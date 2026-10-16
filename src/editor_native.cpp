#include "dve/editor_native.hpp"
#include "dve/editor_exact_cad.hpp"
#include "dve/voxel_metric.hpp"
#include "dve/asset_cooker.hpp"
#include "dve/editor_import_workflow.hpp"
#include "dve/editor_project.hpp"
#include "dve/editor_runtime_settings.hpp"
#include "dve/editor_midi.hpp"
#include "dve/editor_prefab.hpp"
#include "dve/dvox.hpp"
#include "dve/editor_ui_zoom.hpp"
#include "dve/game_script.hpp"
#include "dve/print_export.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/collision_proxy.hpp"
#include "dve/fragment.hpp"

#include <atomic>
#include <condition_variable>
#include <thread>
#include <fstream>
#include <iterator>
#include <mutex>
#include <algorithm>
#include <vector>
#include <bit>
#include <array>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <map>
#include <set>

namespace dve::editor {
namespace {
// Reasons shown for commands this editor build does not implement (menu, Command Center, status).
constexpr std::string_view kAudioImportUnavailable =
    "Not connected in this editor yet: this build has no audio importer behind Audio > Import Audio";
constexpr std::string_view kCreatePolygonBlocked = "Stop Play or Simulate before creating polygon meshes.";
constexpr std::string_view kCookUnavailable =
    "Not connected in this editor yet: cook content with the dve_cook_* command-line tools";
constexpr std::string_view kPackageUnavailable =
    "Not connected in this editor yet: package the game with the dve_pack command-line tool";
constexpr std::string_view kDocsUnavailable =
    "Not in this editor yet: the documentation is in the engine's docs/ folder";
constexpr std::string_view kRebuildBrickmapsUnavailable =
    "Nothing to rebuild: this viewport draws voxels directly (brickmaps are built by the runtime renderer)";
constexpr std::string_view kImportedPivotFixed =
    "An imported model's pivot stays at the model origin so Reimport lands in place";
// Reimport places new parts relative to the import group's transform, so its pivot must stay at
// the model origin.
bool imported_group(const EditorObject& group) {
    return std::any_of(group.components.begin(), group.components.end(),
                       [](const Component& c) { return c.type == kImportSettingsComponent; });
}
} // namespace

namespace {

// The controller always lays out and hit-tests in logical pixels at 1x. UI zoom
// (`editor.ui_scale`) is applied uniformly by the host (logical -> physical mapping plus
// a font rasterized at the zoomed size), so layout code must not multiply by it again.
constexpr float kLogicalLayoutScale = 1.0F;

float point_line_distance(float px, float py, float ax, float ay, float bx, float by) noexcept {
    const float dx = bx - ax;
    const float dy = by - ay;
    const float lengthSquared = dx * dx + dy * dy;
    if (!(lengthSquared > 0.0001F)) return std::hypot(px - ax, py - ay);
    const float t = std::clamp(((px - ax) * dx + (py - ay) * dy) / lengthSquared, 0.0F, 1.0F);
    return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}

std::string prefab_filename_component(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const unsigned char c : text) {
        if (std::isalnum(c)) result.push_back(static_cast<char>(c));
        else if ((c == ' ' || c == '-' || c == '_') && !result.empty() && result.back() != '_') result.push_back('_');
    }
    while (!result.empty() && result.back() == '_') result.pop_back();
    return result.empty() ? std::string("Prefab") : result;
}

std::string lowercase(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

std::vector<std::string> search_tokens(std::string_view query) {
    std::vector<std::string> result;
    std::string token;
    for (const unsigned char c : lowercase(query)) {
        if (std::isspace(c)) {
            if (!token.empty()) { result.push_back(std::move(token)); token.clear(); }
        } else token.push_back(static_cast<char>(c));
    }
    if (!token.empty()) result.push_back(std::move(token));
    return result;
}

int fuzzy_field_score(std::string_view field, std::string_view token) {
    if (field == token) return 0;
    if (field.starts_with(token)) return 8;
    if (const std::size_t position = field.find(token); position != std::string_view::npos)
        return 20 + static_cast<int>(std::min<std::size_t>(position, 40U));
    std::size_t cursor = 0U;
    int gap = 0;
    for (const char c : field) {
        if (cursor < token.size() && c == token[cursor]) ++cursor;
        else if (cursor > 0U && cursor < token.size()) ++gap;
    }
    return cursor == token.size() ? 60 + std::min(gap, 60) : -1;
}

int command_center_score(std::string_view query, std::initializer_list<std::string_view> fields) {
    const std::vector<std::string> tokens = search_tokens(query);
    if (tokens.empty()) return 100;
    std::vector<std::string> normalized;
    normalized.reserve(fields.size());
    for (std::string_view field : fields) normalized.push_back(lowercase(field));
    int total = 0;
    for (const std::string& token : tokens) {
        int best = -1;
        for (std::size_t index = 0; index < normalized.size(); ++index) {
            int candidate = fuzzy_field_score(normalized[index], token);
            if (candidate >= 0) candidate += static_cast<int>(index) * 4;
            if (candidate >= 0 && (best < 0 || candidate < best)) best = candidate;
        }
        if (best < 0) return -1;
        total += best;
    }
    return total;
}

Int3 add_int3(Int3 a, Int3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }

std::string shortcut_profile_name(std::string_view value) {
    if (value == "unity") return "Unity Familiar";
    if (value == "unreal") return "Unreal Familiar";
    if (value == "accessible") return "Accessibility One-Handed";
    if (value == "custom") return "Blank Custom";
    return "DVE Default";
}

camera::CameraCinematicPreset cinematic_preset_from_id(std::string_view id) noexcept {
    if (id == "academy" || id == "camera.preset_academy") return camera::CameraCinematicPreset::AcademyClassic;
    if (id == "imax143" || id == "camera.preset_imax143") return camera::CameraCinematicPreset::Imax143;
    if (id == "imax190" || id == "camera.preset_imax190") return camera::CameraCinematicPreset::Imax190;
    if (id == "scope239" || id == "camera.preset_scope239") return camera::CameraCinematicPreset::Scope239;
    if (id == "anamorphic" || id == "camera.preset_anamorphic") return camera::CameraCinematicPreset::VintageAnamorphic;
    if (id == "fisheye" || id == "camera.preset_fisheye") return camera::CameraCinematicPreset::FisheyeAction;
    if (id == "split_diopter" || id == "camera.preset_split_diopter") return camera::CameraCinematicPreset::SplitDiopter;
    if (id == "bleach_bypass" || id == "camera.preset_bleach_bypass") return camera::CameraCinematicPreset::BleachBypass;
    if (id == "seventies" || id == "camera.preset_seventies") return camera::CameraCinematicPreset::SeventiesFilm;
    return camera::CameraCinematicPreset::Neutral;
}

camera::CameraFilmbackPreset filmback_preset_from_id(std::string_view id) noexcept {
    if (id == "super16" || id == "camera.filmback_super16") return camera::CameraFilmbackPreset::Super16;
    if (id == "super35" || id == "camera.filmback_super35") return camera::CameraFilmbackPreset::Super35;
    if (id == "full_frame" || id == "camera.filmback_full_frame") return camera::CameraFilmbackPreset::FullFrame35;
    if (id == "anamorphic35" || id == "camera.filmback_anamorphic35") return camera::CameraFilmbackPreset::Anamorphic35;
    if (id == "imax15" || id == "camera.filmback_imax15") return camera::CameraFilmbackPreset::Imax15Perf;
    if (id == "imax_digital" || id == "camera.filmback_imax_digital") return camera::CameraFilmbackPreset::ImaxDigital;
    return camera::CameraFilmbackPreset::Custom;
}

std::string_view cinematic_preset_setting_id(camera::CameraCinematicPreset preset) noexcept {
    switch (preset) {
        case camera::CameraCinematicPreset::Neutral: return "neutral";
        case camera::CameraCinematicPreset::AcademyClassic: return "academy";
        case camera::CameraCinematicPreset::Imax143: return "imax143";
        case camera::CameraCinematicPreset::Imax190: return "imax190";
        case camera::CameraCinematicPreset::Scope239: return "scope239";
        case camera::CameraCinematicPreset::VintageAnamorphic: return "anamorphic";
        case camera::CameraCinematicPreset::FisheyeAction: return "fisheye";
        case camera::CameraCinematicPreset::SplitDiopter: return "split_diopter";
        case camera::CameraCinematicPreset::BleachBypass: return "bleach_bypass";
        case camera::CameraCinematicPreset::SeventiesFilm: return "seventies";
    }
    return "neutral";
}

std::string_view filmback_setting_id(camera::CameraFilmbackPreset preset) noexcept {
    switch (preset) {
        case camera::CameraFilmbackPreset::Custom: return "custom";
        case camera::CameraFilmbackPreset::Super16: return "super16";
        case camera::CameraFilmbackPreset::Super35: return "super35";
        case camera::CameraFilmbackPreset::FullFrame35: return "full_frame";
        case camera::CameraFilmbackPreset::Anamorphic35: return "anamorphic35";
        case camera::CameraFilmbackPreset::Imax15Perf: return "imax15";
        case camera::CameraFilmbackPreset::ImaxDigital: return "imax_digital";
    }
    return "custom";
}

std::string pointer_input_name(PointerButton button) {
    switch (button) {
        case PointerButton::Primary: return "mouse1";
        case PointerButton::Secondary: return "mouse2";
        case PointerButton::Auxiliary: return "mouse3";
        case PointerButton::Extra1: return "mouse4";
        case PointerButton::NoButton: break;
    }
    return {};
}

} // namespace

namespace {
EditorWorkspace prepare_startup_workspace(EditorWorkspace workspace, const std::filesystem::path& userPath,
                                          const std::filesystem::path& projectPath) {
    const auto load = [&](SettingScope scope, const std::filesystem::path& path) {
        if (path.empty() || !std::filesystem::exists(path)) return;
        std::string error;
        if (!workspace.settings().load_scope_file(scope, path, &error))
            workspace.log().add(EditorLogLevel::Warning, setting_scope_name(scope) + " settings load: " + error);
    };
    load(SettingScope::User, userPath);
    load(SettingScope::Project, projectPath);
    return workspace;
}
} // namespace

NativeEditorController::NativeEditorController(EditorWorkspace workspace, std::filesystem::path userSettingsPath,
                                               std::filesystem::path projectSettingsPath)
    : workspace_(prepare_startup_workspace(std::move(workspace), userSettingsPath, projectSettingsPath)),
      materials_(EditorMaterialLibrary::make_default()), userSettingsPath_(std::move(userSettingsPath)),
      projectSettingsPath_(std::move(projectSettingsPath)),
      audioMixer_(RuntimeSettingsReader(workspace_.settings()).integer("audio.sample_rate")) {
    apply_settings_to_runtime();
    const SettingValue defaultPresetValue = workspace_.settings().value("camera.default_cinematic_preset");
    const std::string defaultPresetId = std::get_if<std::string>(&defaultPresetValue)
        ? std::get<std::string>(defaultPresetValue) : std::string("neutral");
    const CinematicCameraScope initialScope = cinematicCameraPanel_.scope();
    cinematicCameraPanel_.set_scope(CinematicCameraScope::ProjectDefault);
    cinematicCameraPanel_.profile_for_scope(std::nullopt) =
        camera::camera_cinematic_preset(cinematic_preset_from_id(defaultPresetId));
    cinematicCameraPanel_.set_scope(initialScope);
    if (!workspace_.document().objects().empty()) {
        workspace_.select_object(workspace_.document().objects().begin()->first);
        frame_selection();
    }
    (void)create_camera_rig_from_view("Editor Camera");
    configure_ai_assistant();
    spriteAuthoringPanel_.resize(width_, height_);
    startupWorkspaceState_ = capture_workspace_state();
}

bool NativeEditorController::start_play_session(EditorMode mode) {
    if (playSession_.active()) {
        set_status("A play-in-editor session is already active", true);
        return false;
    }
    EditorPlaySessionConfig config = play_session_settings(workspace_.settings());
    cameraTargetHistory_.clear();
    config.projectRoot = projectRoot_;
    const std::filesystem::path conventionalScript = projectRoot_ / "scripts" / "main.lua";
    if (std::filesystem::exists(conventionalScript)) config.startupScript = conventionalScript;
    cancel_text_edit();
    cancel_polygon_preview();
    if (gizmoDragging_) finish_gizmo_drag(true);
    cancel_voxel_slice();
    if (voxelBoolean_.active()) cancel_voxel_boolean("Boolean preview closed: Play or Simulate started");
    cancel_scatter("Scatter preview closed: Play or Simulate started");
    cancel_voxel_join("Join preview closed: Play or Simulate started");
    floodFillArmed_ = false;  // an armed Flood Fill must not edit the scene from a Play click
    pivotEditing_ = false;
    close_context_menu();
    close_top_level_menu();
    activePointerCommand_.clear();
    gizmoDragging_ = false;
    gizmoChanges_.clear();
    voxelStrokeActive_ = false;
    brushStroking_ = false;
    brushResizing_ = false;
    brushPending_.clear();
    brushEraseIds_.clear();
    brushCursor_.reset();
    marquee_ = {};
    hierarchyDrag_ = {};
    reset_voxel_selection_state();  // Play edits voxels; Stop restores the document
    prePlayEditorCamera_ = camera_;
    possessedPlayCamera_ = camera_;
    playCameraPossessed_ = mode == EditorMode::Play;
    if (playCameraPossessed_ && selectedCameraRig_) {
        if (const camera::CameraRig* rig = cameraDirector_.find_rig(*selectedCameraRig_)) {
            apply_camera_pose(camera_, rig->authoredPose);
            possessedPlayCamera_ = camera_;
        }
    }
    std::string error;
    const bool started = playSession_.start(
        workspace_, materials_, mode, std::move(config),
        [this](EditorLogLevel level, std::string text) { workspace_.log().add(level, std::move(text)); },
        &error);
    if (!started) {
        if (prePlayEditorCamera_) camera_ = *prePlayEditorCamera_;
        prePlayEditorCamera_.reset();
        possessedPlayCamera_.reset();
        playCameraPossessed_ = false;
        set_status(error.empty() ? "Could not start play-in-editor" : error, true);
        return false;
    }
    recompute_layout();  // the Scatter settings panel (shown with the brush tool) gives way to Play
    set_status(mode == EditorMode::Play ? "Play-in-editor started" : "Simulation started");
    refresh_menu_state();
    return true;
}

bool NativeEditorController::stop_play_session(bool acceptChanges) {
    std::string error;
    const bool stopped = acceptChanges
        ? playSession_.accept_runtime_changes(workspace_, &error)
        : playSession_.stop(workspace_, &error);
    if (!stopped) {
        set_status(error.empty() ? "Could not stop play-in-editor" : error, true);
        return false;
    }
    if (prePlayEditorCamera_) camera_ = *prePlayEditorCamera_;
    prePlayEditorCamera_.reset();
    possessedPlayCamera_.reset();
    playCameraPossessed_ = false;
    cameraTargetHistory_.clear();
    workspace_.prune_selection();
    recompute_layout();
    refresh_menu_state();
    set_status(acceptChanges ? "Runtime changes accepted" : "Returned to edit mode");
    return true;
}

void NativeEditorController::toggle_play_camera_possession() {
    if (!playSession_.active() || playSession_.mode() != EditorMode::Play) {
        set_status("Camera possession is available only during Play", true);
        return;
    }
    if (playCameraPossessed_) {
        possessedPlayCamera_ = camera_;
        if (prePlayEditorCamera_) camera_ = *prePlayEditorCamera_;
        playCameraPossessed_ = false;
        set_status("Ejected to editor camera");
    } else {
        if (possessedPlayCamera_) camera_ = *possessedPlayCamera_;
        playCameraPossessed_ = true;
        set_status("Possessed play camera");
    }
}

void NativeEditorController::route_play_key(std::string_view normalized, bool pressed) {
    if (!playSession_.active() || normalized.empty()) return;
    playSession_.set_action_pressed("key." + std::string(normalized), pressed);
    const auto alias = [&](std::string_view action) { playSession_.set_action_pressed(std::string(action), pressed); };
    if (normalized == "w" || normalized == "up") alias("move_forward");
    else if (normalized == "s" || normalized == "down") alias("move_backward");
    else if (normalized == "a" || normalized == "left") alias("move_left");
    else if (normalized == "d" || normalized == "right") alias("move_right");
    else if (normalized == "space") alias("jump");
    else if (normalized == "e") alias("interact");
    else if (normalized == "leftshift" || normalized == "rightshift" || normalized == "shift") alias("sprint");
    else if (normalized == "leftctrl" || normalized == "rightctrl" || normalized == "control") alias("crouch");
    refresh_play_input_axes();
}

void NativeEditorController::refresh_play_input_axes() {
    if (!playSession_.active()) return;
    const auto& actions = playSession_.input().actions;
    const auto active = [&](std::string_view name) {
        const auto it = actions.find(name);
        return it != actions.end() && it->second;
    };
    const float moveX = (active("move_right") ? 1.0F : 0.0F) - (active("move_left") ? 1.0F : 0.0F);
    const float moveY = (active("move_forward") ? 1.0F : 0.0F) - (active("move_backward") ? 1.0F : 0.0F);
    playSession_.set_axis("move_x", std::clamp(moveX + gamepadMove_[0], -1.0F, 1.0F));
    playSession_.set_axis("move_y", std::clamp(moveY + gamepadMove_[1], -1.0F, 1.0F));
    playSession_.set_axis("look_x", gamepadLook_[0]);
    playSession_.set_axis("look_y", gamepadLook_[1]);
}

void NativeEditorController::set_gamepad_axes(std::array<float, 2> move, std::array<float, 2> look) {
    gamepadMove_ = move;
    gamepadLook_ = look;
    refresh_play_input_axes();
}

void NativeEditorController::clear_navigation_input() {
    cancel_voxel_authoring_preview();
    finish_slice_pointer(true);
    cancel_polygon_preview();
    cancel_mesh_authoring_drag();
    if (gizmoDragging_) finish_gizmo_drag(true);
    heldShortcutGestures_.clear();
    activePointerCommand_.clear();
    dragButton_ = PointerButton::NoButton;
    reset_camera_navigation();
    playSession_.clear_input();
    gamepadMove_ = {};
    gamepadLook_ = {};
    spriteLevelInput_ = {};
}

void NativeEditorController::reset_camera_navigation() noexcept {
    cameraNavigationFilter_.reset();
    pendingCameraLook_ = {};
    navigationCameraRig_ = selectedCameraRig_;
}

void NativeEditorController::configure_ai_assistant(std::filesystem::path projectRoot) {
    const bool explicitProject = !projectRoot.empty();
    if (projectRoot.empty()) projectRoot = std::filesystem::current_path();
    std::error_code projectError;
    auto nextRoot = std::filesystem::weakly_canonical(projectRoot, projectError);
    if (projectError) nextRoot = projectRoot.lexically_normal();
    const bool activateWorkspace = explicitProject && (!workspaceProjectConfigured_ || nextRoot != projectRoot_);
    if (activateWorkspace && workspaceProjectConfigured_) {
        std::string error;
        if (!save_workspace_state(&error)) workspace_.log().add(EditorLogLevel::Warning, "Workspace save: " + error);
    }
    stop_live_mcp_host();
    (void)finish_asset_scan();
    projectRoot_ = std::move(nextRoot);
    assetDatabase_.set_project_root(projectRoot_);
    std::string assetIndexError;
    if (!assetDatabase_.load(&assetIndexError) && !assetIndexError.empty())
        workspace_.log().add(EditorLogLevel::Warning, "Asset index load: " + assetIndexError);
    ai::DveAiBridgeOptions options;
    options.projectRoot = projectRoot_;
    aiBridge_ = std::make_unique<ai::DveAiBridge>(std::move(options));
    register_editor_ai_tools(*aiBridge_, workspace_);
    std::string registrationError;
    (void)aiBridge_->registry().add({
        "dve.editor.live_mcp_status",
        "Live editor MCP status",
        "Report the private IPC endpoint state owned by this running editor without exposing its authentication token.",
        ai::JsonValue::Object{{"type", "object"}, {"additionalProperties", false}},
        ai::AiToolRisk::ReadOnly,
        true
    }, [this](const ai::JsonValue&) {
        const auto current = live_mcp_status();
        return ai::AiToolCallResult{ai::AiCallStatus::Completed, ai::JsonValue::Object{
            {"supported", current.supported},
            {"running", current.running},
            {"instance_id", current.instanceId},
            {"descriptor_path", current.descriptorPath.generic_string()},
            {"endpoint_kind", current.endpointPath.empty() ? "" : "private-local-ipc"},
            {"connected_clients", static_cast<double>(current.connectedClients)},
            {"pending_requests", static_cast<double>(current.pendingRequests)},
            {"accepted_connections", static_cast<double>(current.acceptedConnections)},
            {"completed_requests", static_cast<double>(current.completedRequests)},
            {"rejected_connections", static_cast<double>(current.rejectedConnections)},
            {"last_error", current.lastError}
        }, "Live editor MCP host status", {}};
    }, &registrationError);
    auto transport = std::make_unique<ai::CurlAiHttpTransport>();
    auto client = std::make_unique<ai::OpenAiResponsesClient>(*aiBridge_, std::move(transport));
    workspace_.ai_assistant().attach(std::move(client), aiBridge_.get(), &aiTasks_);
    if (explicitProject) workspaceProjectConfigured_ = true;
    if (activateWorkspace) restore_workspace_state();
}


std::vector<const EditorAssetRecord*> NativeEditorController::asset_browser_rows() const {
    EditorAssetQuery query = assetBrowserState_.query;
    query.limit = 2048U;
    return assetDatabase_.query(query);
}

bool NativeEditorController::refresh_asset_database(bool announce) {
    (void)finish_asset_scan();
    EditorAssetScanReport report;
    std::string error;
    // Thumbnails are written a little per frame from update() instead of inside the
    // scan, which could otherwise block the editor for seconds on a first scan.
    EditorAssetScanOptions options;
    options.generateThumbnails = false;
    if (!assetDatabase_.scan(&report, &error, options)) {
        if (announce) set_status(error.empty() ? "Asset scan failed" : error, true, 7.0F);
        return false;
    }
    thumbnailBacklog_ = true;
    if (assetBrowserState_.selectedId && !std::as_const(assetDatabase_).find(*assetBrowserState_.selectedId))
        assetBrowserState_.selectedId.reset();
    assetBrowserState_.firstVisible = 0U;
    restore_workspace_asset_selection();
    recompute_layout();
    if (announce) {
        set_status("Assets indexed: " + std::to_string(report.indexed) +
                   " (added " + std::to_string(report.added) +
                   ", changed " + std::to_string(report.changed) +
                   ", moved " + std::to_string(report.moved) + ")");
    }
    for (const std::string& warning : report.warnings) workspace_.log().add(EditorLogLevel::Warning, warning);
    return true;
}

struct NativeEditorController::AssetScanJob {
    std::mutex mutex;
    std::condition_variable finished;
    bool done{};
    bool success{};
    bool announce{};
    std::string error;
    EditorAssetScanReport report;
    EditorAssetDatabase database;      // a copy of the editor's database, scanned on the worker
    std::uint64_t baseRevision{};      // the editor database's revision when the copy was made
};

bool NativeEditorController::start_asset_scan(bool announce) {
    if (assetScanInFlight_) {
        // Files may have changed after the running scan read them; scan again when it ends.
        assetRescanQueued_ = true;
        assetRescanAnnounce_ = assetRescanAnnounce_ || announce;
        return true;
    }
    // The worker scans a copy, so the panel keeps showing (and querying) the current records
    // while it runs; asset_scan_tick() swaps the result in. Anything that edits the database
    // meanwhile calls finish_asset_scan() first, so the two never write the index together.
    auto job = std::make_shared<AssetScanJob>();
    job->announce = announce;
    job->database = assetDatabase_;
    job->baseRevision = assetDatabase_.revision();
    std::string submitError;
    const auto task = assetScanTasks_.submit("Scan assets", [job,wake=wakeCallback_](EditorTaskContext&) {
        EditorAssetScanReport report;
        std::string error;
        bool success = false;
        try {
            // Thumbnails are written a little per frame from update(), not inside the scan.
            EditorAssetScanOptions options;
            options.generateThumbnails = false;
            success = job->database.scan(&report, &error, options);
        } catch (const std::exception& exception) {
            error = exception.what();
        }
        {
            std::lock_guard lock(job->mutex);
            job->done = true;
            job->success = success;
            job->error = std::move(error);
            job->report = std::move(report);
        }
        job->finished.notify_all();
        if(wake) wake();
    }, &submitError);
    if (!task) return refresh_asset_database(announce);  // no worker: scan here, as before
    assetScanInFlight_ = std::move(job);
    assetRescanQueued_ = false;
    assetRescanAnnounce_ = false;
    if (announce) set_status("Scanning assets...", false, 30.0F);
    return true;
}

void NativeEditorController::apply_asset_scan(AssetScanJob& job) {
    if (!job.success) {
        if (job.announce) set_status(job.error.empty() ? "Asset scan failed" : job.error, true, 7.0F);
        else if (!job.error.empty()) workspace_.log().add(EditorLogLevel::Warning, "Asset scan failed: " + job.error);
        return;
    }
    if (assetDatabase_.revision() != job.baseRevision ||
        assetDatabase_.project_root() != job.database.project_root()) {
        // The database changed underneath the scan (finish_asset_scan() normally prevents
        // this): drop the result and scan the current state again.
        assetRescanQueued_ = true;
        assetRescanAnnounce_ = assetRescanAnnounce_ || job.announce;
        return;
    }
    assetDatabase_ = std::move(job.database);
    thumbnailBacklog_ = true;
    if (assetBrowserState_.selectedId && !std::as_const(assetDatabase_).find(*assetBrowserState_.selectedId))
        assetBrowserState_.selectedId.reset();
    // Keep the user's scroll position unless the list got shorter than it.
    if (assetBrowserState_.firstVisible >= assetDatabase_.records().size()) assetBrowserState_.firstVisible = 0U;
    restore_workspace_asset_selection();
    recompute_layout();
    const EditorAssetScanReport& report = job.report;
    if (job.announce) {
        set_status("Assets indexed: " + std::to_string(report.indexed) +
                   " (added " + std::to_string(report.added) +
                   ", changed " + std::to_string(report.changed) +
                   ", moved " + std::to_string(report.moved) + ")");
    }
    for (const std::string& warning : report.warnings) workspace_.log().add(EditorLogLevel::Warning, warning);
}

void NativeEditorController::asset_scan_tick() {
    if (assetScanInFlight_) {
        {
            std::lock_guard lock(assetScanInFlight_->mutex);
            if (!assetScanInFlight_->done) return;
        }
        const auto job = std::move(assetScanInFlight_);
        apply_asset_scan(*job);
    }
    if (assetRescanQueued_) (void)start_asset_scan(assetRescanAnnounce_);
}

bool NativeEditorController::finish_asset_scan() {
    if (!assetScanInFlight_) return false;
    {
        std::unique_lock lock(assetScanInFlight_->mutex);
        assetScanInFlight_->finished.wait(lock, [&] { return assetScanInFlight_->done; });
    }
    const auto job = std::move(assetScanInFlight_);
    apply_asset_scan(*job);
    return true;
}

EditorAssetDatabase& NativeEditorController::asset_database() {
    (void)finish_asset_scan();
    return assetDatabase_;
}

bool NativeEditorController::start_live_mcp_host(ai::LiveEditorMcpHostOptions options,
                                                  std::string* error) {
    if (!aiBridge_) configure_ai_assistant(options.projectRoot);
    if (liveMcpHost_ && liveMcpHost_->status().running) return true;
    if (options.projectRoot.empty()) options.projectRoot = aiBridge_->project_root();
    auto host = std::make_unique<ai::LiveEditorMcpHost>(*aiBridge_, std::move(options));
    std::string localError;
    if (!host->start(&localError)) {
        set_status(localError, true, 8.0F);
        if (error) *error = localError;
        return false;
    }
    const auto current = host->status();
    liveMcpHost_ = std::move(host);
    refresh_menu_state();
    set_status("Live MCP host active: " + current.descriptorPath.string(), false, 6.0F);
    return true;
}

void NativeEditorController::stop_live_mcp_host() noexcept {
    if (!liveMcpHost_) return;
    liveMcpHost_->stop();
    liveMcpHost_.reset();
    refresh_menu_state();
}

ai::LiveEditorMcpHostStatus NativeEditorController::live_mcp_status() const {
    if (liveMcpHost_) return liveMcpHost_->status();
    ai::LiveEditorMcpHostStatus status;
#if defined(__unix__) || defined(__APPLE__)
    status.supported = true;
#endif
    return status;
}

std::filesystem::path NativeEditorController::find_default_text3d_font() const {
    if (const char* configured = std::getenv("DVE_TEXT3D_DEFAULT_FONT"); configured && *configured) {
        const std::filesystem::path candidate(configured);
        if (std::filesystem::is_regular_file(candidate)) return candidate;
    }
    const std::array<std::filesystem::path, 3> preferred{{
        projectRoot_ / "assets/fonts/Default.ttf",
        projectRoot_ / "assets/fonts/default.ttf",
        projectRoot_ / "fonts/Default.ttf",
    }};
    for (const auto& candidate : preferred)
        if (std::filesystem::is_regular_file(candidate)) return candidate;
    std::vector<std::filesystem::path> fonts;
    std::error_code ec;
    const auto directory = projectRoot_ / "assets/fonts";
    if (std::filesystem::is_directory(directory, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
            if (ec) break;
            std::string extension = lowercase(entry.path().extension().string());
            if (entry.is_regular_file() && extension == ".ttf") fonts.push_back(entry.path());
        }
    }
    std::sort(fonts.begin(), fonts.end());
    return fonts.empty() ? std::filesystem::path{} : fonts.front();
}

std::filesystem::path NativeEditorController::find_default_gabor_asset() const {
    const std::filesystem::path directory = projectRoot_ / "assets/volumes";
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) return {};
    std::vector<std::filesystem::path> cooked;
    std::vector<std::filesystem::path> ply;
    std::vector<std::filesystem::path> pyramids;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, ec)) {
        if (ec) break;
        if (entry.is_directory()) {
            if (std::filesystem::is_regular_file(entry.path()/"root.primitives_pyr0.ply"))
                pyramids.push_back(entry.path());
            continue;
        }
        if (!entry.is_regular_file()) continue;
        const std::string extension = lowercase(entry.path().extension().string());
        if (extension == ".dgabor") cooked.push_back(entry.path());
        else if (extension == ".ply") ply.push_back(entry.path());
    }
    std::sort(cooked.begin(), cooked.end());
    std::sort(ply.begin(), ply.end());
    std::sort(pyramids.begin(), pyramids.end());
    if (!cooked.empty()) return cooked.front();
    if (!pyramids.empty()) return pyramids.front();
    return ply.empty() ? std::filesystem::path{} : ply.front();
}

bool NativeEditorController::create_text3d(std::filesystem::path fontPath, std::string text,
                                           Text3DCookOptions options) {
    if (fontPath.empty()) fontPath = find_default_text3d_font();
    if (fontPath.empty()) {
        set_status("No project TrueType font found. Add assets/fonts/Default.ttf or set DVE_TEXT3D_DEFAULT_FONT.", true, 6.0F);
        return false;
    }
    if (!text3dAuthoring_.begin_create(projectRoot_, std::move(fontPath), std::move(text), options)) {
        set_status("3D text preview failed: " + text3dAuthoring_.preview().error, true, 6.0F);
        return false;
    }
    text3dAuthoring_.set_output_asset_path(
        std::filesystem::path("assets/text") /
        ("text_" + std::to_string(text3dAuthoring_.preview().asset->contentHash) + ".dtext"));
    const CommandResult result = text3dAuthoring_.commit(
        workspace_, "3D Text", make_rigid_transform(camera_.target, {}));
    if (!result.success) {
        set_status(result.message, true, 6.0F);
        return false;
    }
    recompute_layout();
    set_status("Created Slug 3D text");
    return true;
}

bool NativeEditorController::create_gabor_volume(std::filesystem::path sourcePath) {
    GaborVolumeAsset asset;
    std::filesystem::path sourceRelative;
    std::filesystem::path cookedRelative;
    bool wroteCooked = false;
    std::string loadError;

    if (sourcePath.empty()) {
        asset.name = "Gabor Volume";
        GaborVolumePrimitive seed;
        seed.center = {};
        seed.scale = {0.75F, 0.75F, 0.75F};
        seed.opacity = 0.35F;
        seed.extent = 3.0F;
        seed.albedo = {0.72F, 0.78F, 0.86F};
        asset.primitives.push_back(seed);
        asset.material.densityMultiplier = 1.0F;
        asset.recompute_bounds_and_hash();
        cookedRelative = std::filesystem::path("assets/volumes") /
            ("gabor_" + std::to_string(asset.contentHash) + ".dgabor");
        const auto cookedAbsolute = projectRoot_ / cookedRelative;
        if (!write_dgabor(cookedAbsolute, asset, &loadError)) {
            set_status("Could not create Gabor asset: " + loadError, true, 6.0F);
            return false;
        }
        wroteCooked = true;
    } else {
        std::error_code ec;
        std::filesystem::path absolute = sourcePath.is_absolute() ? sourcePath : projectRoot_/sourcePath;
        absolute = std::filesystem::weakly_canonical(absolute, ec);
        const auto root = std::filesystem::weakly_canonical(projectRoot_, ec);
        const auto relative = std::filesystem::relative(absolute, root, ec);
        if (ec || relative.empty() || relative.is_absolute() ||
            std::any_of(relative.begin(), relative.end(), [](const auto& part) { return part == ".."; })) {
            set_status("Gabor source must be inside the project", true, 6.0F);
            return false;
        }
        sourceRelative = relative;
        GaborImportResult imported;
        if (std::filesystem::is_directory(absolute)) imported = import_gabor_pyramid(absolute);
        else if (lowercase(absolute.extension().string()) == ".dgabor") imported = read_dgabor(absolute);
        else if (lowercase(absolute.extension().string()) == ".ply") imported = import_gabor_ply(absolute);
        else {
            set_status("Gabor import supports .dgabor, .ply, or a pyramid directory", true, 6.0F);
            return false;
        }
        if (!imported) {
            set_status("Could not import Gabor volume: " + imported.error, true, 6.0F);
            return false;
        }
        asset = std::move(*imported.asset);
        if (lowercase(absolute.extension().string()) == ".dgabor") {
            cookedRelative = sourceRelative;
        } else {
            cookedRelative = std::filesystem::path("assets/volumes") /
                (asset.name + "_" + std::to_string(asset.contentHash) + ".dgabor");
            if (!write_dgabor(projectRoot_/cookedRelative, asset, &loadError)) {
                set_status("Could not cook Gabor volume: " + loadError, true, 6.0F);
                return false;
            }
            wroteCooked = true;
        }
    }

    const EditorObjectId id = workspace_.document().allocate_object_id();
    EditorObject object(id, asset.name.empty() ? "Gabor Volume" : asset.name);
    object.transform = make_rigid_transform(camera_.target, {});
    object.flags.structural = false;
    object.flags.collisionEnabled = false;
    object.flags.decorative = true;
    object.sourceAsset = cookedRelative;
    object.importRecipe = sourceRelative;
    object.gaborVolume = asset;
    object.voxels = std::make_unique<VoxelObject>(id);
    const CommandResult result = workspace_.commands().execute(
        workspace_.document(), std::make_unique<AddObjectCommand>(std::move(object), "Add Gabor volume"));
    if (!result.success) {
        if (wroteCooked) {
            std::error_code removeError;
            std::filesystem::remove(projectRoot_/cookedRelative, removeError);
        }
        set_status(result.message, true, 6.0F);
        return false;
    }
    workspace_.select_object(id);
    recompute_layout();
    frame_selection();
    set_status("Created Gabor volume: " + asset.name);
    return true;
}

bool NativeEditorController::edit_selected_text3d() {
    const auto selected = workspace_.selected_object();
    const EditorObject* object = selected ? workspace_.document().find_object(*selected) : nullptr;
    if (!object || !object->text3d) {
        set_status("Select a 3D text object first", true);
        return false;
    }
    if (!text3dAuthoring_.begin_edit(projectRoot_, *object)) {
        set_status("Could not open 3D text authoring session", true);
        return false;
    }
    set_status("3D text authoring session opened");
    return true;
}

bool NativeEditorController::commit_text3d_authoring() {
    if (!text3dAuthoring_.active()) {
        set_status("No 3D text authoring session is active", true);
        return false;
    }
    const CommandResult result = text3dAuthoring_.commit(workspace_);
    set_status(result.success ? "Committed 3D text changes" : result.message, !result.success, 6.0F);
    if (result.success) recompute_layout();
    return result.success;
}

void NativeEditorController::cancel_text3d_authoring() {
    text3dAuthoring_.cancel();
    set_status("Cancelled 3D text changes");
}

bool NativeEditorController::submit_ai_prompt(std::string prompt) {
    if (prompt.empty()) return false;
    const bool ok = workspace_.ai_assistant().send(std::move(prompt));
    set_status(ok ? "AI Assistant request submitted" : workspace_.ai_assistant().last_error(), !ok, ok ? 3.0F : 6.0F);
    return ok;
}

std::optional<std::string_view> NativeEditorController::open_menu() const noexcept {
    if (!openMenu_) return std::nullopt;
    return std::string_view(*openMenu_);
}

std::vector<MenuAction> NativeEditorController::menu_actions(std::string_view menuName) const {
    return workspace_.menus().menu(menuName, showAdvancedMenus_);
}

namespace {
struct PaletteKeyHasher {
    std::uint64_t h{0x9E3779B97F4A7C15ULL};
    void u64(std::uint64_t v) noexcept {
        h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6U) + (h >> 2U);
        h *= 0xBF58476D1CE4E5B9ULL;
        h ^= h >> 31U;
    }
    void text(std::string_view v) noexcept { u64(std::hash<std::string_view>{}(v)); u64(v.size()); }
};
} // namespace

// The palette scores every command, setting, panel, asset and scene object, and is
// requested on every pointer move and every frame while open. Results are cached
// until something they are derived from changes: the query, the limit, favorites and
// recents, the menu registry and asset database revisions, the advanced-settings
// toggle, or the scene objects' ids and names (hashed, which is far cheaper than
// scoring them).
std::vector<CommandPaletteResult> NativeEditorController::command_palette_results(std::size_t limit) const {
    PaletteKeyHasher key;
    key.text(commandPaletteQuery_);
    key.u64(limit);
    key.u64(favoriteCommandIds_.size());
    for (const std::string& id : favoriteCommandIds_) key.text(id);
    key.u64(recentCommandIds_.size());
    for (const std::string& id : recentCommandIds_) key.text(id);
    key.u64(workspace_.menus().revision());
    key.u64(assetDatabase_.revision());
    key.u64(settingsPanel_.includeAdvanced ? 1U : 0U);
    key.u64(workspace_.settings().definitions().size());
    if (!commandPaletteQuery_.empty()) {
        const auto& objects = workspace_.document().objects();
        key.u64(objects.size());
        for (const auto& [id, object] : objects) {
            key.u64(id);
            key.text(object.name);
        }
    }
    if (!paletteCacheValid_ || key.h != paletteCacheKey_) {
        paletteCache_ = build_command_palette_results(limit);
        paletteCacheKey_ = key.h;
        paletteCacheValid_ = true;
        ++paletteCacheRebuilds_;
    }
    return paletteCache_;
}

std::vector<CommandPaletteResult> NativeEditorController::build_command_palette_results(std::size_t limit) const {
    enum class Provider : std::uint8_t { All, Commands, Settings, Panels, Assets, Objects, Documentation };
    Provider provider = Provider::All;
    std::string query = commandPaletteQuery_;
    if (!query.empty()) {
        switch (query.front()) {
            case '>': provider = Provider::Commands; break;
            case '@': provider = Provider::Settings; break;
            case '/': provider = Provider::Panels; break;
            case '#': provider = Provider::Assets; break;
            case ':': provider = Provider::Objects; break;
            case '?': provider = Provider::Documentation; break;
            default: break;
        }
        if (provider != Provider::All) {
            query.erase(query.begin());
            while (!query.empty() && query.front() == ' ') query.erase(query.begin());
        }
    }

    struct ScoredResult { int score{}; std::size_t order{}; CommandPaletteResult result; };
    std::vector<ScoredResult> scored;
    std::size_t order = 0U;
    const auto append = [&](int score, CommandPaletteResult result) {
        if (score >= 0) scored.push_back({score, order++, std::move(result)});
    };
    const auto commandResult = [&](const MenuAction& action, int score) {
        const std::string breadcrumb = action.menu +
            (action.section.empty() ? std::string{} : " > " + action.section);
        append(score + (action.enabled ? 0 : 500),
               {CommandPaletteResultKind::Command, action.id, action.label, breadcrumb,
                action.description, action.shortcut, action.disabledReason, action.enabled,
                favoriteCommandIds_.contains(action.id)});
    };

    if (query.empty() && (provider == Provider::All || provider == Provider::Commands)) {
        int priority = 0;
        for (const std::string& id : favoriteCommandIds_) {
            if (const MenuAction* action = std::as_const(workspace_.menus()).find(id)) commandResult(*action, priority++);
        }
        for (const std::string& id : recentCommandIds_) {
            if (std::any_of(scored.begin(), scored.end(), [&](const ScoredResult& item) {
                    return item.result.kind == CommandPaletteResultKind::Command && item.result.id == id;
                })) continue;
            if (const MenuAction* action = std::as_const(workspace_.menus()).find(id)) commandResult(*action, 100 + priority++);
        }
        for (std::string_view id : {"file.save", "edit.undo", "sprite.tile_world", "sprite.diagnostics",
                                    "render.diagnostics3d", "physics.play", "build.validate", "help.shortcuts"}) {
            if (std::any_of(scored.begin(), scored.end(), [&](const ScoredResult& item) {
                    return item.result.kind == CommandPaletteResultKind::Command && item.result.id == id;
                })) continue;
            if (const MenuAction* action = std::as_const(workspace_.menus()).find(id)) commandResult(*action, 250 + priority++);
        }
    } else if (provider == Provider::All || provider == Provider::Commands) {
        for (const MenuAction& action : workspace_.menus().search(query, 64)) {
            const int score = command_center_score(query,
                {action.label, action.id, action.menu, action.section, action.description, action.shortcut});
            commandResult(action, score);
        }
    }

    if ((provider == Provider::All || provider == Provider::Settings) && !query.empty()) {
        const auto settings = workspace_.settings().search(
            query, settingsPanel_.includeAdvanced, settings_capabilities(), 64);
        for (const SettingSearchResult& match : settings) {
            const SettingDefinition& definition = *match.definition;
            const std::string breadcrumb = "Settings > " + definition.category +
                (definition.section.empty() ? std::string{} : " > " + definition.section);
            const int score = command_center_score(query,
                {definition.label, definition.id, definition.category, definition.section, definition.description});
            append(score + 4, {CommandPaletteResultKind::Setting, definition.id, definition.label,
                               breadcrumb, definition.description, {}, {}, true, false});
        }
    }

    struct PanelEntry { std::string_view label; std::string_view action; std::string_view detail; };
    static constexpr std::array<PanelEntry, 17> panels{{
        {"Scene Hierarchy", "window.toggle_hierarchy", "Browse and organize scene objects."},
        {"Inspector", "window.toggle_inspector", "Edit the selected object's components and properties."},
        {"Assets", "window.toggle_assets", "Search indexed project content and dependencies."},
        {"Synthesizer", "window.toggle_synth", "Open the native synthesizer."},
        {"Audio Mixer", "window.toggle_audio", "Open buses, meters, and event audition."},
        {"Audio Event Graph", "window.toggle_audio_event", "Author interactive audio events."},
        {"AI Assistant", "window.toggle_ai_assistant", "Open the project-aware assistant panel."},
        {"Control Rig Editor", "window.toggle_control_rig", "Author control rigs and constraints."},
        {"Chiptune Tracker", "window.toggle_chiptune", "Compose tracker music and procedural SFX."},
        {"Sprite Editor", "sprite.editor", "Import, slice, animate, and annotate sprites."},
        {"Pixel Art Studio", "sprite.pixel_art", "Paint indexed or RGBA layered animation frames and publish sprites."},
        {"Multi-Part Sprite Rig", "sprite.rig2d", "Author bones, sprite parts, IK, variants, and bake plans."},
        {"Animation State Graph", "sprite.graph", "Author sprite animation states and transitions."},
        {"Tile World Editor", "sprite.tile_world", "Author tilesets, levels, collision, objects, and play tests."},
        {"Sprite Diagnostics", "sprite.diagnostics", "Inspect batches, overdraw, residency, references, and budgets."},
        {"3D Rendering Diagnostics", "render.diagnostics3d", "Inspect meshes, pipelines, LOD, skinning, voxel materials, shadows, lighting, occlusion, and depth complexity."},
        {"Voxel Material Policy", "render.voxel_material_policy", "Choose baked, single-material, deferred-palette, or hybrid voxel material rendering."}
    }};
    if (provider == Provider::Panels || ((provider == Provider::All) && !query.empty())) {
        for (const PanelEntry& panel : panels) {
            const int score = command_center_score(query, {panel.label, panel.action, panel.detail, "panel window workspace"});
            if (score < 0) continue;
            const MenuAction* action = std::as_const(workspace_.menus()).find(panel.action);
            append(score + 2, {CommandPaletteResultKind::Panel, std::string(panel.action), std::string(panel.label),
                               "Window > Panels", std::string(panel.detail),
                               action ? action->shortcut : std::string{}, action ? action->disabledReason : std::string{},
                               action == nullptr || action->enabled, false});
        }
    }

    if ((provider == Provider::Assets || provider == Provider::Documentation || provider == Provider::All) &&
        (!query.empty() || provider != Provider::All)) {
        EditorAssetQuery assetQuery;
        // Provider-level fuzzy matching is richer than the asset browser's literal filter, so
        // retrieve a bounded inventory and rank it here.
        assetQuery.text.clear();
        assetQuery.limit = 512U;
        for (const EditorAssetRecord* asset : assetDatabase_.query(assetQuery)) {
            const bool documentation = asset->kind == EditorAssetKind::Document ||
                asset->relativePath.extension() == ".md" || asset->relativePath.extension() == ".html" ||
                asset->relativePath.extension() == ".pdf";
            if (provider == Provider::Documentation && !documentation) continue;
            if (provider == Provider::Assets && documentation) continue;
            const int score = command_center_score(query,
                {asset->displayName, asset->relativePath.generic_string(), to_string(asset->kind), to_string(asset->health)});
            if (score < 0 && !query.empty()) continue;
            append((score < 0 ? 100 : score) + (documentation ? 10 : 8),
                   {documentation ? CommandPaletteResultKind::Documentation : CommandPaletteResultKind::Asset,
                    asset->id, asset->displayName,
                    documentation ? "Help > Project Documentation" : "Assets > " + std::string(to_string(asset->kind)),
                    asset->relativePath.generic_string() + " | " + std::string(to_string(asset->health)),
                    {}, {}, true, false});
        }
    }

    if ((provider == Provider::Objects || provider == Provider::All) && !query.empty()) {
        for (const auto& [id, object] : workspace_.document().objects()) {
            const std::string idText = std::to_string(id);
            const int score = command_center_score(query, {object.name, idText, "scene object entity"});
            if (score < 0) continue;
            append(score + 6, {CommandPaletteResultKind::SceneObject, idText, object.name,
                               "Scene > Objects", "Select and frame object " + idText + ".",
                               {}, {}, true, false});
        }
    }

    std::stable_sort(scored.begin(), scored.end(), [](const ScoredResult& a, const ScoredResult& b) {
        if (a.score != b.score) return a.score < b.score;
        return a.order < b.order;
    });
    std::vector<CommandPaletteResult> result;
    result.reserve(std::min(limit, scored.size()));
    for (std::size_t index = 0; index < scored.size() && index < limit; ++index)
        result.push_back(std::move(scored[index].result));
    return result;
}

NativeCommandPaletteLayout NativeEditorController::command_palette_layout() const {
    NativeCommandPaletteLayout result;
    const float scale = kLogicalLayoutScale;
    const int margin = std::max(20, static_cast<int>(30.0F * scale));
    result.panel.width = std::min(820, std::max(560, width_ - margin * 2));
    result.panel.height = std::min(560, std::max(360, height_ - margin * 2));
    result.panel.x = (width_ - result.panel.width) / 2;
    result.panel.y = std::max(32, (height_ - result.panel.height) / 3);
    result.searchBox = {result.panel.x + 16, result.panel.y + 16, result.panel.width - 32,
                        std::max(34, static_cast<int>(38.0F * scale))};
    const int hintHeight = std::max(30, static_cast<int>(34.0F * scale));
    result.hintBar = {result.panel.x + 12, result.panel.y + result.panel.height - hintHeight - 8,
                      result.panel.width - 24, hintHeight};
    const int rowHeight = std::max(44, static_cast<int>(50.0F * scale));
    const int bodyY = result.searchBox.y + result.searchBox.height + 10;
    const int bodyBottom = result.hintBar.y - 6;
    const std::size_t visible = static_cast<std::size_t>(
        std::max(1, (bodyBottom - bodyY) / rowHeight));
    const auto rows = command_palette_results(64);
    if (commandPaletteSelection_ >= visible)
        result.firstVisibleRow = commandPaletteSelection_ - visible + 1U;
    result.firstVisibleRow = std::min(result.firstVisibleRow, rows.size());
    for (std::size_t index = result.firstVisibleRow;
         index < rows.size() && result.rows.size() < visible; ++index) {
        result.rows.push_back({result.panel.x + 12,
                               bodyY + static_cast<int>(result.rows.size()) * rowHeight,
                               result.panel.width - 24, rowHeight});
    }
    return result;
}

void NativeEditorController::persist_menu_state() noexcept {
    if (menuStatePath_.empty()) return;
    EditorMenuUserState state;
    state.showAdvancedCommands = showAdvancedMenus_;
    state.favoriteActionIds.assign(favoriteCommandIds_.begin(), favoriteCommandIds_.end());
    state.recentActionIds.assign(recentCommandIds_.begin(), recentCommandIds_.end());
    std::string error;
    if (!state.save(menuStatePath_, &error) && !error.empty())
        workspace_.log().add(EditorLogLevel::Warning, "Menu state save: " + error);
}

void NativeEditorController::configure_menu_state(std::filesystem::path path) {
    menuStatePath_ = std::move(path);
    favoriteCommandIds_.clear();
    recentCommandIds_.clear();
    showAdvancedMenus_ = false;
    if (!menuStatePath_.empty() && std::filesystem::exists(menuStatePath_)) {
        std::string error;
        if (auto state = EditorMenuUserState::load(menuStatePath_, &error)) {
            showAdvancedMenus_ = state->showAdvancedCommands;
            for (const std::string& id : state->favoriteActionIds)
                if (std::as_const(workspace_.menus()).find(id)) favoriteCommandIds_.insert(id);
            for (const std::string& id : state->recentActionIds)
                if (std::as_const(workspace_.menus()).find(id)) recentCommandIds_.push_back(id);
        } else if (!error.empty()) workspace_.log().add(EditorLogLevel::Warning, "Menu state load: " + error);
    }
    refresh_menu_state();
}

void NativeEditorController::configure_user_settings(std::filesystem::path path) {
    userSettingsPath_ = std::move(path);
    if (userSettingsPath_.empty() || !std::filesystem::exists(userSettingsPath_)) return;
    std::string error;
    if (!workspace_.settings().load_scope_file(SettingScope::User, userSettingsPath_, &error)) {
        workspace_.log().add(EditorLogLevel::Warning, "User settings load: " + error);
        return;
    }
    apply_settings_to_runtime();
    synchronize_menu_shortcuts();
    recompute_layout();
}

bool NativeEditorController::save_user_settings(std::string* error) const {
    if (userSettingsPath_.empty()) return true;
    return workspace_.settings().save_scope_file(SettingScope::User, userSettingsPath_, error);
}

float NativeEditorController::ui_zoom() const noexcept {
    return snap_ui_zoom(workspace_.preferences().uiScale);
}

float NativeEditorController::effective_ui_zoom() const noexcept {
    return std::min(ui_zoom(), std::max(kUiZoomMin, uiZoomWindowLimit_));
}

void NativeEditorController::set_ui_zoom_window_limit(float maximumZoom) noexcept {
    uiZoomWindowLimit_ = std::isfinite(maximumZoom) ? std::clamp(maximumZoom, kUiZoomMin, kUiZoomMax) : kUiZoomMax;
}

bool NativeEditorController::set_ui_zoom(float requested) {
    const float zoom = snap_ui_zoom(requested);
    std::string error;
    // UI zoom is a per-user accessibility preference: hotkeys write the User layer and drop any
    // transient Session override so the settings row, hotkeys and hosts all read one value.
    (void)workspace_.settings().clear(SettingScope::Session, kUiZoomSettingId);
    if (!workspace_.settings().set(SettingScope::User, kUiZoomSettingId, static_cast<double>(zoom), &error)) {
        set_status(error, true);
        return false;
    }
    workspace_.synchronize_preferences_from_settings();
    std::string message = "UI zoom " + format_ui_zoom_percent(ui_zoom());
    if (workspace_.settings().has_override(SettingScope::Project, kUiZoomSettingId))
        message += " (Project settings override the User value)";
    else if (effective_ui_zoom() + 1.0e-3F < ui_zoom())
        message += " (window too small; showing " + format_ui_zoom_percent(effective_ui_zoom()) + ")";
    if (!save_user_settings(&error)) {
        set_status(message + "; save failed: " + error, true);
        return false;
    }
    set_status(std::move(message));
    return true;
}

int NativeEditorController::keyboard_key_count() const noexcept {
    const SettingValue value = workspace_.settings().value(kKeyboardKeysSettingId);
    if (const auto* text = std::get_if<std::string>(&value)) {
        int keys = kPianoDefaultKeyCount;
        const auto [end, ec] = std::from_chars(text->data(), text->data() + text->size(), keys);
        (void)end;
        if (ec == std::errc{}) return snap_piano_key_count(keys);
    }
    return kPianoDefaultKeyCount;
}

bool NativeEditorController::set_keyboard_key_count(int keys) {
    const int snapped = snap_piano_key_count(keys);
    std::string error;
    (void)workspace_.settings().clear(SettingScope::Session, kKeyboardKeysSettingId);
    if (!workspace_.settings().set(SettingScope::User, kKeyboardKeysSettingId, std::to_string(snapped), &error)) {
        set_status(error, true);
        return false;
    }
    synthPanel_.set_keyboard_key_count(keyboard_key_count());
    chiptunePanel_.set_keyboard_key_count(keyboard_key_count());
    refresh_menu_state();
    std::string message = "Piano keyboard: " + std::to_string(keyboard_key_count()) + " keys";
    if (keyboard_key_count() != snapped) message += " (Project settings override the User value)";
    if (!save_user_settings(&error)) {
        set_status(message + "; save failed: " + error, true);
        return false;
    }
    set_status(std::move(message));
    return true;
}

bool NativeEditorController::step_ui_zoom(int direction) {
    return set_ui_zoom(dve::editor::step_ui_zoom(ui_zoom(), direction));
}

void NativeEditorController::record_command_use(std::string_view actionId) {
    if (actionId.empty() || actionId == "help.command_palette") return;
    const std::string id(actionId);
    std::erase(recentCommandIds_, id);
    recentCommandIds_.push_front(id);
    while (recentCommandIds_.size() > 24U) recentCommandIds_.pop_back();
    persist_menu_state();
}

bool NativeEditorController::activate_command_palette_selection() {
    const auto results = command_palette_results(64);
    if (results.empty()) return false;
    const std::size_t index = std::min(commandPaletteSelection_, results.size() - 1U);
    const CommandPaletteResult selected = results[index];
    if (!selected.enabled) {
        set_status(selected.disabledReason.empty() ? "That command is not available in the current context"
                                                   : selected.disabledReason, true, 6.0F);
        return false;
    }
    commandPaletteOpen_ = false;
    commandPaletteQuery_.clear();
    commandPaletteSelection_ = 0U;
    if (selected.kind == CommandPaletteResultKind::Command || selected.kind == CommandPaletteResultKind::Panel)
        return dispatch_action(selected.id);
    if (selected.kind == CommandPaletteResultKind::Setting) {
        const SettingDefinition* definition = workspace_.settings().find(selected.id);
        if (!definition) return false;
        const bool userScoped = definition->category == "General" || definition->category == "Input" ||
                                definition->category == "Accessibility";
        open_settings(userScoped ? SettingScope::User : SettingScope::Project, definition->category);
        settingsPanel_.searchQuery = definition->label;
        settingsPanel_.selectedRow = 0U;
        return true;
    }
    if (selected.kind == CommandPaletteResultKind::SceneObject) {
        EditorObjectId id{};
        const char* begin = selected.id.data();
        const char* end = begin + selected.id.size();
        const auto parsed = std::from_chars(begin, end, id);
        if (parsed.ec != std::errc{} || parsed.ptr != end || !workspace_.document().find_object(id)) return false;
        workspace_.select_object(id);
        frame_selection();
        set_status("Selected " + selected.label);
        return true;
    }
    const EditorAssetRecord* asset = std::as_const(assetDatabase_).find(selected.id);
    if (!asset) return false;
    assetBrowserState_.selectedId = asset->id;
    assetBrowserState_.query.text.clear();
    assetBrowserState_.firstVisible = 0U;
    bottomTab_ = BottomPanelTab::Assets;
    workspace_.set_panel_visible(PanelId::Assets, true);
    recompute_layout();
    set_status((selected.kind == CommandPaletteResultKind::Documentation ? "Located documentation: " : "Located asset: ") +
               asset->relativePath.generic_string(), false, 6.0F);
    return true;
}


GaborVolumeRenderSettings NativeEditorController::gabor_render_settings() const noexcept {
    GaborVolumeRenderSettings settings;
    const auto boolValue=[&](std::string_view id,bool fallback){const auto value=workspace_.settings().value(id);if(const auto* item=std::get_if<bool>(&value))return *item;return fallback;};
    const auto stringValue=[&](std::string_view id,std::string fallback){const auto value=workspace_.settings().value(id);if(const auto* item=std::get_if<std::string>(&value))return *item;return fallback;};
    const auto intValue=[&](std::string_view id,std::int64_t fallback){const auto value=workspace_.settings().value(id);if(const auto* item=std::get_if<std::int64_t>(&value))return *item;return fallback;};
    const auto floatValue=[&](std::string_view id,double fallback){const auto value=workspace_.settings().value(id);if(const auto* item=std::get_if<double>(&value))return *item;return fallback;};
    settings.enabled=boolValue("render.gabor.enabled",true);
    const auto mode=stringValue("render.gabor.mode","emission_absorption");
    settings.mode=mode=="absorption"?GaborVolumeMode::AbsorptionPreview:mode=="scattering"?GaborVolumeMode::ScatteringExperimental:GaborVolumeMode::EmissionAbsorption;
    const auto quality=stringValue("render.gabor.quality","medium");
    settings.quality=quality=="low"?GaborVolumeQuality::Low:quality=="high"?GaborVolumeQuality::High:quality=="cinematic"?GaborVolumeQuality::Cinematic:GaborVolumeQuality::Medium;
    settings.continuousLod=boolValue("render.gabor.continuous_lod",true);
    settings.temporalAccumulation=boolValue("render.gabor.temporal_accumulation",true);
    settings.castVolumeShadows=boolValue("render.gabor.cast_shadows",true);
    settings.receiveSceneShadows=boolValue("render.gabor.receive_shadows",true);
    settings.maximumPrimitivesPerTile=static_cast<std::uint32_t>(std::clamp<std::int64_t>(intValue("render.gabor.max_primitives_per_tile",512),1,65536));
    settings.lodBias=static_cast<float>(floatValue("render.gabor.lod_bias",0.0));
    return settings;
}


VoxelMaterialPolicyConfig NativeEditorController::voxel_material_policy_config() const noexcept {
    VoxelMaterialPolicyConfig policy;
    const auto boolValue = [&](std::string_view id, bool fallback) {
        const SettingValue value = workspace_.settings().value(id);
        if (const auto* item = std::get_if<bool>(&value)) return *item;
        return fallback;
    };
    const auto stringValue = [&](std::string_view id, std::string fallback) {
        const SettingValue value = workspace_.settings().value(id);
        if (const auto* item = std::get_if<std::string>(&value)) return *item;
        return fallback;
    };
    const auto intValue = [&](std::string_view id, std::int64_t fallback) {
        const SettingValue value = workspace_.settings().value(id);
        if (const auto* item = std::get_if<std::int64_t>(&value)) return *item;
        return fallback;
    };
    const auto floatValue = [&](std::string_view id, double fallback) {
        const SettingValue value = workspace_.settings().value(id);
        if (const auto* item = std::get_if<double>(&value)) return *item;
        return fallback;
    };

    const std::string mode = stringValue("voxel.material_mode", "hybrid");
    policy.mode = mode == "baked" ? VoxelMaterialMode::BakedProperties
        : mode == "single" ? VoxelMaterialMode::SingleMaterial
        : mode == "deferred" ? VoxelMaterialMode::DeferredPalette
        : VoxelMaterialMode::Hybrid;
    const std::string platform = stringValue("voxel.material_platform", "high_end_desktop");
    policy.platform = platform == "editor" ? VoxelMaterialPlatformProfile::Editor
        : platform == "console" ? VoxelMaterialPlatformProfile::Console
        : platform == "integrated_gpu" ? VoxelMaterialPlatformProfile::IntegratedGpu
        : platform == "mobile" ? VoxelMaterialPlatformProfile::Mobile
        : platform == "server" ? VoxelMaterialPlatformProfile::DedicatedServer
        : VoxelMaterialPlatformProfile::HighEndDesktop;
    const std::string overflow = stringValue("voxel.material_overflow", "bake");
    policy.overflowPolicy = overflow == "dominant" ? VoxelMaterialPaletteOverflowPolicy::DominantMaterial
        : overflow == "recook" ? VoxelMaterialPaletteOverflowPolicy::RequestRecook
        : overflow == "reject" ? VoxelMaterialPaletteOverflowPolicy::Reject
        : VoxelMaterialPaletteOverflowPolicy::BakeProperties;
    policy.deferredMaximumDistance = static_cast<float>(
        std::max(0.0, floatValue("voxel.material_deferred_distance", 40.0)));
    policy.deferredMaximumLod = static_cast<std::uint32_t>(std::clamp<std::int64_t>(
        intValue("voxel.material_deferred_lod", 1), 0, 16));
    policy.maximumPaletteSlots = static_cast<std::uint8_t>(std::clamp<std::int64_t>(
        intValue("voxel.material_palette_slots", 4), 1, 4));
    policy.retainBakedFallback = boolValue("voxel.material_retain_baked", true);
    policy.retainSingleMaterialFallback = boolValue("voxel.material_retain_single", true);
    policy.allowRuntimeSwitching = boolValue("voxel.material_runtime_switching", true);
    policy.enableFourWayBlending = boolValue("voxel.material_four_way", true) &&
        policy.maximumPaletteSlots >= 4U;
    policy.preferDeferredForDestructible = boolValue("voxel.material_prefer_destructible", true);
    policy.fallBackUnderMemoryPressure = boolValue("voxel.material_memory_fallback", true);
    return policy;
}

std::vector<GaborVolumeFramePlan> NativeEditorController::gabor_frame_plans(float projectedDiameterPixels) const {
    std::vector<GaborVolumeFramePlan> plans;const auto settings=gabor_render_settings();
    for(const auto& [id,object]:workspace_.document().objects()){
        (void)id;if(!object.flags.visible||!object.gaborVolume)continue;
        plans.push_back(plan_gabor_volume_frame(*object.gaborVolume,settings,projectedDiameterPixels));
    }
    return plans;
}

std::uint32_t NativeEditorController::settings_capabilities() const noexcept {
    // The native canvas has no active DVE Vulkan device. Compiled offscreen RHI
    // support alone must not advertise a usable Vulkan viewport owner.
    std::uint32_t result = SettingCapabilityEditor | SettingCapabilityAudio;
#if defined(DVE_GEOMETRY_MODE_VOXEL) || defined(DVE_GEOMETRY_MODE_HYBRID)
    result |= SettingCapabilityVoxel;
#endif
#if defined(DVE_GEOMETRY_MODE_POLYGON) || defined(DVE_GEOMETRY_MODE_HYBRID)
    result |= SettingCapabilityPolygon;
#endif
#if defined(DVE_HAVE_LUA)
    result |= SettingCapabilityLua;
#endif
#if defined(DVE_HAVE_JOLT)
    result |= SettingCapabilityJolt;
#endif
    return result;
}

std::vector<std::string> NativeEditorController::settings_categories() const {
    return workspace_.settings().categories(settings_capabilities());
}

std::vector<const SettingDefinition*> NativeEditorController::settings_rows() const {
    std::vector<const SettingDefinition*> result;
    if (!settingsPanel_.searchQuery.empty()) {
        for (const SettingSearchResult& match : workspace_.settings().search(
                 settingsPanel_.searchQuery, settingsPanel_.includeAdvanced,
                 settings_capabilities(), 128)) {
            if (settingsPanel_.changedOnly && !workspace_.settings().differs_from_default(match.definition->id))
                continue;
            result.push_back(match.definition);
        }
        return result;
    }
    if (settingsPanel_.changedOnly) {
        for (const SettingSearchResult& match : workspace_.settings().changed(
                 settingsPanel_.includeAdvanced, settings_capabilities())) {
            if (match.definition->category == settingsPanel_.selectedCategory)
                result.push_back(match.definition);
        }
        return result;
    }
    return workspace_.settings().category(settingsPanel_.selectedCategory,
                                           settingsPanel_.includeAdvanced,
                                           settings_capabilities());
}

NativeSettingsModalLayout NativeEditorController::settings_modal_layout() const {
    NativeSettingsModalLayout result;
    const float scale = kLogicalLayoutScale;
    const int margin = std::max(20, static_cast<int>(28.0F * scale));
    // Preferred minimum 700x500, but never larger than the (logical) window: at the
    // 640x480 minimum layout the modal used to hang off the left and bottom edges.
    result.panel.width = std::clamp(width_ - margin * 2, std::min(700, width_ - 8), 1120);
    result.panel.height = std::clamp(height_ - margin * 2, std::min(500, height_ - 8), 780);
    result.panel.x = (width_ - result.panel.width) / 2;
    result.panel.y = (height_ - result.panel.height) / 2;
    const int header = std::max(82, static_cast<int>(88.0F * scale));
    const int footer = std::max(58, static_cast<int>(64.0F * scale));
    const int detailHeight = std::max(88, static_cast<int>(98.0F * scale));
    const int categoryWidth = std::clamp(static_cast<int>(205.0F * scale), 170, 280);
    const int rowHeight = std::max(27, static_cast<int>(30.0F * scale));

    const int tabWidth = std::max(76, static_cast<int>(86.0F * scale));
    // The title gets its own cell on the top row; the scope tabs follow it (they used to be
    // drawn over the title). On very narrow modals the title shrinks (elided, tooltip on hover).
    const int tabsWidth = static_cast<int>(result.scopeTabs.size()) * (tabWidth + 6);
    const int titleWidth = std::clamp(result.panel.width - 32 - tabsWidth - 12, 60, 200);
    result.title = {result.panel.x + 16, result.panel.y + 10, titleWidth, 28};
    const int tabsLeft = result.title.x + result.title.width + 12;
    for (std::size_t index = 0; index < result.scopeTabs.size(); ++index)
        result.scopeTabs[index] = {tabsLeft + static_cast<int>(index) * (tabWidth + 6),
                                   result.panel.y + 10, tabWidth, 28};
    result.advancedToggle = {result.panel.x + result.panel.width - 144, result.panel.y + 44, 124, 30};
    result.changedToggle = {result.advancedToggle.x - 124 - 8, result.panel.y + 44, 124, 30};
    result.searchBox = {result.panel.x + categoryWidth + 24, result.panel.y + 44,
                        std::max(180, result.panel.width - categoryWidth - 318), 30};
    // Keep the search box clear of the Changed toggle on narrow windows.
    result.searchBox.width = std::max(96, std::min(result.searchBox.width, result.changedToggle.x - 8 - result.searchBox.x));

    const int bodyY = result.panel.y + header;
    const int detailY = result.panel.y + result.panel.height - footer - detailHeight;
    const int bodyHeight = std::max(rowHeight, detailY - bodyY);
    const auto categories = settings_categories();
    const std::size_t visibleCategories = static_cast<std::size_t>(std::max(1, bodyHeight / rowHeight));
    for (std::size_t index = 0; index < categories.size() && index < visibleCategories; ++index)
        result.categoryRows.push_back({result.panel.x + 8, bodyY + static_cast<int>(index) * rowHeight,
                                       categoryWidth - 16, rowHeight});

    const auto rows = settings_rows();
    const std::size_t visibleRows = static_cast<std::size_t>(std::max(1, bodyHeight / rowHeight));
    if (settingsPanel_.selectedRow >= visibleRows)
        result.firstVisibleSetting = settingsPanel_.selectedRow - visibleRows + 1U;
    result.firstVisibleSetting = std::min(result.firstVisibleSetting, rows.size());
    for (std::size_t index = result.firstVisibleSetting;
         index < rows.size() && result.settingRows.size() < visibleRows; ++index) {
        result.settingRows.push_back({result.panel.x + categoryWidth + 10,
                                      bodyY + static_cast<int>(result.settingRows.size()) * rowHeight,
                                      result.panel.width - categoryWidth - 20, rowHeight});
    }

    result.detailPanel = {result.panel.x + categoryWidth + 10, detailY + 4,
                          result.panel.width - categoryWidth - 20, detailHeight - 8};
    const int buttonWidth = std::max(104, static_cast<int>(116.0F * scale));
    const int buttonHeight = std::max(30, static_cast<int>(34.0F * scale));
    result.resetSettingButton = {result.panel.x + 16,
                                 result.panel.y + result.panel.height - buttonHeight - 12,
                                 buttonWidth + 18, buttonHeight};
    result.resetCategoryButton = {result.resetSettingButton.x + result.resetSettingButton.width + 8,
                                  result.resetSettingButton.y, buttonWidth + 18, buttonHeight};
    result.applyButton = {result.panel.x + result.panel.width - buttonWidth - 16,
                          result.panel.y + result.panel.height - buttonHeight - 12,
                          buttonWidth, buttonHeight};
    result.discardButton = {result.applyButton.x - buttonWidth - 10, result.applyButton.y,
                            buttonWidth, buttonHeight};
    return result;
}

std::vector<ShortcutSearchResult> NativeEditorController::shortcut_rows() const {
    return workspace_.shortcuts().search(shortcutPanel_.searchQuery, shortcutPanel_.contextFilter,
                                         settings_capabilities(), 256);
}

NativeShortcutModalLayout NativeEditorController::shortcut_modal_layout() const {
    NativeShortcutModalLayout result;
    const float scale = kLogicalLayoutScale;
    const int margin = std::max(20, static_cast<int>(28.0F * scale));
    result.panel.width = std::min(1120, std::max(720, width_ - margin * 2));
    result.panel.height = std::min(760, std::max(480, height_ - margin * 2));
    result.panel.x = (width_ - result.panel.width) / 2;
    result.panel.y = (height_ - result.panel.height) / 2;
    const int header = std::max(76, static_cast<int>(82.0F * scale));
    const int footer = std::max(48, static_cast<int>(54.0F * scale));
    const int rowHeight = std::max(25, static_cast<int>(29.0F * scale));
    result.searchBox = {result.panel.x + 18, result.panel.y + 42, result.panel.width - 36, 28};
    result.profilePrevious = {result.panel.x + 18, result.panel.y + 10, 30, 26};
    result.profileNext = {result.panel.x + 250, result.panel.y + 10, 30, 26};
    result.contextPrevious = {result.panel.x + result.panel.width - 280, result.panel.y + 10, 30, 26};
    result.contextNext = {result.panel.x + result.panel.width - 48, result.panel.y + 10, 30, 26};
    const int bodyY = result.panel.y + header;
    const int bodyHeight = result.panel.height - header - footer;
    const auto rows = shortcut_rows();
    const std::size_t visible = static_cast<std::size_t>(std::max(1, bodyHeight / rowHeight));
    if (shortcutPanel_.selectedRow >= visible)
        result.firstVisibleRow = shortcutPanel_.selectedRow - visible + 1U;
    result.firstVisibleRow = std::min(result.firstVisibleRow, rows.size());
    for (std::size_t index = result.firstVisibleRow; index < rows.size() && result.rows.size() < visible; ++index)
        result.rows.push_back({result.panel.x + 12, bodyY + static_cast<int>(result.rows.size()) * rowHeight,
                               result.panel.width - 24, rowHeight});
    result.resetButton = {result.panel.x + 18, result.panel.y + result.panel.height - 40, 132, 30};
    result.closeButton = {result.panel.x + result.panel.width - 118, result.panel.y + result.panel.height - 40, 100, 30};
    return result;
}

std::vector<ShortcutContext> NativeEditorController::active_shortcut_contexts() const {
    if (shortcutContextOverride_) return {*shortcutContextOverride_, ShortcutContext::Global};
    if (pending_destructive_confirmation() || settingsPanel_.open || shortcutPanel_.open || commandPaletteOpen_)
        return {ShortcutContext::ModalDialog, ShortcutContext::Global};
    if (textEdit_.kind != TextEditKind::Inactive) return {ShortcutContext::TextField, ShortcutContext::Global};
    if (workspace_.mode() != EditorMode::Edit) return {ShortcutContext::PlayMode, ShortcutContext::Global};
    std::vector<ShortcutContext> result;
    if (audioPanel_.open() || audioEventPanel_.open() || synthPanel_.open() || chiptunePanel_.open())
        result.push_back(ShortcutContext::AudioEditor);
    if (focusRegion_ == EditorFocusRegion::Viewport) {
        if (selectionDomain_==SelectionDomain::Polygons) result.push_back(ShortcutContext::PolygonEditor);
        if (activePointerCommand_ == "viewport.look")
            result.push_back(ShortcutContext::FlyNavigation);
        if (activeTool_ == EditorToolId::AddVoxel || activeTool_ == EditorToolId::RemoveVoxel ||
            activeTool_ == EditorToolId::PaintMaterial || activeTool_ == EditorToolId::Box ||
            activeTool_ == EditorToolId::Beam || activeTool_ == EditorToolId::Anchor)
            result.push_back(ShortcutContext::VoxelEditor);
        result.push_back(ShortcutContext::Camera);
        result.push_back(ShortcutContext::Viewport);
    } else if (focusRegion_ == EditorFocusRegion::BottomPanel && bottomTab_ == BottomPanelTab::Assets) {
        result.push_back(ShortcutContext::AssetBrowser);
    }
    result.push_back(ShortcutContext::Global);
    return result;
}


void NativeEditorController::set_camera_mode(camera::CameraRigMode mode) noexcept {
    clear_navigation_input();
    cameraMode_ = mode;
    std::string value = "orbit";
    switch (mode) {
        case camera::CameraRigMode::FreeFly: value = "free"; break;
        case camera::CameraRigMode::Orbit: value = "orbit"; break;
        case camera::CameraRigMode::Follow: value = "follow"; break;
        case camera::CameraRigMode::ThirdPerson: value = "third_person"; break;
        case camera::CameraRigMode::FirstPerson: value = "first_person"; break;
        case camera::CameraRigMode::Cinematic: value = "cinematic"; break;
        case camera::CameraRigMode::Fixed: value = "orbit"; break;
    }
    (void)workspace_.settings().set(SettingScope::Session, "camera.default_mode", value);
    refresh_menu_state();
}

camera::CameraRigId NativeEditorController::create_camera_rig_from_view(std::string name) {
    camera::CameraRig rig;
    rig.id = nextCameraRigId_++;
    rig.name = name.empty() ? "Camera " + std::to_string(rig.id) : std::move(name);
    rig.mode = camera::CameraRigMode::Cinematic;
    rig.priority = 10;
    rig.outputChannels = 1U;
    rig.authoredPose = to_camera_pose(camera_, static_cast<float>(std::max(1, layout_.viewport.width)) /
                                               static_cast<float>(std::max(1, layout_.viewport.height)));
    rig.lens = rig.authoredPose.lens;
    rig.postProcessWeight = 1.0F;
    const auto authoredFraming = rig.framing;
    const auto authoredCollision = rig.collision;
    configure_camera_rig(rig, workspace_.settings());
    std::string error;
    if (!cameraDirector_.add_or_replace_rig(rig, &error)) {
        set_status(error, true);
        return 0;
    }
    cameraRigSettingsBaselines_.insert_or_assign(rig.id,
        CameraRigSettingsBaseline{authoredFraming, authoredCollision, rig.framing, rig.collision});
    selectedCameraRig_ = rig.id;
    (void)cameraDirector_.force_live(rig.id, true);
    const SettingValue presetValue = workspace_.settings().value("camera.default_cinematic_preset");
    const std::string presetId = std::get_if<std::string>(&presetValue)
        ? std::get<std::string>(presetValue) : std::string("neutral");
    cinematicCameraPanel_.profile_for_rig(rig.id) =
        camera::camera_cinematic_preset(cinematic_preset_from_id(presetId));
    set_status("Created camera rig: " + rig.name);
    return rig.id;
}

void NativeEditorController::save_camera_bookmark(std::size_t index) {
    if (index >= cameraBookmarks_.size()) return;
    cameraBookmarks_[index] = {true, "Bookmark " + std::to_string(index + 1U), camera_};
    set_status("Saved camera bookmark " + std::to_string(index + 1U));
}

bool NativeEditorController::load_camera_bookmark(std::size_t index) noexcept {
    if (index >= cameraBookmarks_.size() || !cameraBookmarks_[index].occupied) return false;
    camera_ = cameraBookmarks_[index].camera;
    set_status("Loaded camera bookmark " + std::to_string(index + 1U));
    return true;
}

void NativeEditorController::open_settings(SettingScope scope, std::string category) {
    clear_navigation_input();
    settingsPanel_.open_for(scope, std::move(category));
    close_top_level_menu();
    close_context_menu();
    set_status(setting_scope_name(scope) + " settings");
}

void NativeEditorController::close_settings(bool applyChanges) {
    if (!settingsPanel_.open) return;
    if (applyChanges) {
        std::string error;
        // UI zoom is a per-user accessibility preference shared with the hotkeys: whichever
        // scope tab the row was edited on, the value is written to (and saved in) the User layer.
        std::optional<SettingValue> stagedZoom;
        bool resetZoom = false;
        if (const auto it = settingsPanel_.stagedValues.find(kUiZoomSettingId); it != settingsPanel_.stagedValues.end()) {
            stagedZoom = it->second;
            settingsPanel_.stagedValues.erase(it);
        }
        if (const auto it = settingsPanel_.stagedClears.find(kUiZoomSettingId); it != settingsPanel_.stagedClears.end()) {
            resetZoom = true;
            settingsPanel_.stagedClears.erase(it);
        }
        if (stagedZoom || resetZoom) {
            (void)workspace_.settings().clear(SettingScope::Session, kUiZoomSettingId);
            (void)workspace_.settings().clear(SettingScope::Project, kUiZoomSettingId);
            if (stagedZoom) {
                if (const auto* zoom = std::get_if<double>(&*stagedZoom))
                    (void)workspace_.settings().set(SettingScope::User, kUiZoomSettingId,
                                                    static_cast<double>(snap_ui_zoom(static_cast<float>(*zoom))));
            }
            else (void)workspace_.settings().clear(SettingScope::User, kUiZoomSettingId);
        }
        if (!settingsPanel_.apply(workspace_.settings(), &error)) {
            set_status(error, true);
            return;
        }
        // Settings typed as free numbers (e.g. 1.1) are stored snapped so the row shows what is used.
        SettingScope zoomSource = SettingScope::User;
        bool zoomInherited = false;
        const SettingValue zoomValue = workspace_.settings().value(kUiZoomSettingId, &zoomSource, &zoomInherited);
        if (const auto* zoom = std::get_if<double>(&zoomValue); zoom && !zoomInherited) {
            const double snapped = static_cast<double>(snap_ui_zoom(static_cast<float>(*zoom)));
            if (snapped != *zoom) (void)workspace_.settings().set(zoomSource, kUiZoomSettingId, snapped);
        }
        apply_settings_to_runtime();
        const bool projectSaved = settingsPanel_.scope != SettingScope::Project || projectSettingsPath_.empty() ||
            workspace_.settings().save_scope_file(SettingScope::Project, projectSettingsPath_, &error);
        if (!projectSaved) {
            set_status(settingsPanel_.status + "; Project settings save failed: " + error, true);
        } else if (!save_user_settings(&error)) {
            set_status(settingsPanel_.status + "; User settings save failed: " + error, true);
        } else set_status(settingsPanel_.status + (audioSettingsPending_ ? "; audio changes pending" : "") + (playSession_.active()
            ? "; simulation configuration takes effect on the next Play session" : ""));
    } else {
        settingsPanel_.discard();
        set_status("Settings changes discarded");
    }
    settingsPanel_.close();
}

void NativeEditorController::open_shortcut_editor() {
    clear_navigation_input();
    shortcutPanel_ = {};
    shortcutPanel_.open = true;
    shortcutPanel_.contextFilter = ShortcutContext::Viewport;
    close_top_level_menu();
    close_context_menu();
    set_status("Keyboard and mouse shortcuts");
}

void NativeEditorController::close_shortcut_editor() noexcept {
    shortcutPanel_ = {};
}

namespace {
// Commands that exist only as Polygon Editor / Material Editor shortcuts (#101 refused them only
// when used). Each is a disabled menu item with this reason.
const std::vector<std::pair<std::string, std::string>>& editor_context_only_commands() {
    static const std::vector<std::pair<std::string, std::string>> kCommands = [] {
        std::vector<std::pair<std::string, std::string>> list;
        const std::string paint = "Not in this editor yet: use the Paint tool (5) or Voxel > Flood Fill to apply the active material.";
        const std::string instances = "Not in this editor yet: material instances and previews; edit materials under Materials > Material Library.";
        list.emplace_back("material.assign", paint);
        list.emplace_back("material.apply", paint);
        for (const char* id : {"material.link", "material.create_instance", "material.create_master", "material.preview", "material.pin_preview"})
            list.emplace_back(id, instances);
        return list;
    }();
    return kCommands;
}
} // namespace

void NativeEditorController::synchronize_menu_shortcuts() noexcept {
    const SettingValue visibleValue = workspace_.settings().value("input.show_shortcuts_in_menus");
    const bool visible = !std::get_if<bool>(&visibleValue) || std::get<bool>(visibleValue);
    const auto& contextOnly = editor_context_only_commands();
    for (const MenuAction& action : workspace_.menus().actions()) {
        // Polygon/Material Editor keys (B, E, M, P, ...) only apply in editors this build does
        // not have; next to a viewport menu they would read as viewport shortcuts (M is the
        // selection domain), so those rows show no key.
        const bool hide = std::any_of(contextOnly.begin(), contextOnly.end(),
                                      [&](const auto& entry) { return entry.first == action.id; });
        (void)workspace_.menus().set_shortcut(action.id, visible && !hide ? workspace_.shortcuts().display_binding(action.id) : std::string{});
    }
}

void NativeEditorController::remember_camera_position() {
    if (previousCamera_ && length_squared(subtract(previousCamera_->position, camera_.position)) < 1.0e-8F &&
        length_squared(subtract(previousCamera_->target, camera_.target)) < 1.0e-8F) return;
    previousCamera_ = camera_;
    nextCamera_.reset();
}


bool NativeEditorController::apply_audio_settings_to_runtime() noexcept {
    const RuntimeSettingsReader s(workspace_.settings());
    audioMixer_.set_stream_preload_milliseconds(s.integer("audio.stream_preload_ms"));
    const auto quality = s.get<std::string>("audio.granular_quality");
    synthesizer().set_granular_runtime_quality(quality == "low" ? audio::GranularRuntimeQuality::Low
        : quality == "medium" ? audio::GranularRuntimeQuality::Medium
        : quality == "ultra" ? audio::GranularRuntimeQuality::Ultra : audio::GranularRuntimeQuality::High);
    auto gains = audioMixer_.user_bus_gains();
    const auto gain = [&](std::string_view id, audio::AudioBusId bus) {
        gains[audio::audio_bus_index(bus)] = std::pow(10.0F, s.number(id) / 20.0F);
    };
    gain("audio.master_gain_db", audio::AudioBusId::Master);
    gain("audio.music_gain_db", audio::AudioBusId::Music);
    gain("audio.effects_gain_db", audio::AudioBusId::Effects);
    gain("audio.dialogue_gain_db", audio::AudioBusId::Dialogue);
    if (!audioMixer_.set_user_bus_gains(gains)) return false;
    const auto range = s.get<std::string>("accessibility.dynamic_range");
    audioMixer_.set_output_policy(s.get<bool>("accessibility.mono_audio"),
        range == "night" ? audio::AudioDynamicRange::Night :
        range == "medium" ? audio::AudioDynamicRange::Medium : audio::AudioDynamicRange::Full);
    const auto spatializerMode = editor::audio_spatializer_mode(workspace_.settings());
    if (spatializerMode != appliedSpatializerMode_) {
        audioMixer_.set_spatializer(editor::make_audio_spatializer(workspace_.settings()));
        appliedSpatializerMode_ = spatializerMode;
        if (spatializerMode == editor::AudioSpatializerMode::SteamAudio) {
            workspace_.log().add(EditorLogLevel::Warning,
                "audio.spatializer is set to Steam Audio, but no native Steam Audio "
                "backend is available in this build; the analytic fallback is active.");
        }
    }
    return true;
}

void NativeEditorController::apply_settings_to_runtime() noexcept {
    std::string settingsError;
    if (!workspace_.settings().validate(&settingsError)) {
        appliedSettingsRevision_ = workspace_.settings().revision();
        set_status("Settings were not activated: " + settingsError, true);
        return;
    }
    workspace_.synchronize_preferences_from_settings();
    if (playSession_.active()) {
        const auto live = play_session_settings(workspace_.settings());
        if (!playSession_.apply_live_settings(live, &settingsError))
            set_status("Simulation settings were not activated: " + settingsError, true);
    }
    const auto readBool = [&](std::string_view id, bool fallback) {
        const SettingValue value = workspace_.settings().value(id);
        if (const auto* item = std::get_if<bool>(&value)) return *item;
        return fallback;
    };
    const auto readFloat = [&](std::string_view id, float fallback) {
        const SettingValue value = workspace_.settings().value(id);
        if (const auto* item = std::get_if<double>(&value)) return static_cast<float>(*item);
        return fallback;
    };
    const auto readInteger = [&](std::string_view id, std::size_t fallback) {
        const SettingValue value = workspace_.settings().value(id);
        if (const auto* item = std::get_if<std::int64_t>(&value))
            return static_cast<std::size_t>(std::max<std::int64_t>(0, *item));
        return fallback;
    };
    const auto readString = [&](std::string_view id, std::string fallback) {
        const SettingValue value = workspace_.settings().value(id);
        if (const auto* item = std::get_if<std::string>(&value)) return *item;
        return fallback;
    };
    const float acceleration = readFloat("camera.input_acceleration", 0.0F);
    const std::string navigationStyle = readString("camera.navigation_style", "dve");
    const bool rawMouse = readBool("input.raw_mouse", true);
    if (workspace_.shortcuts().navigation_style() != navigationStyle || rawMouseRequested_ != rawMouse) {
        clear_navigation_input();
        (void)workspace_.shortcuts().set_navigation_style(navigationStyle);
        rawMouseRequested_ = rawMouse;
    }
    const float smoothing = readFloat("camera.input_smoothing", 0.0F);
    if (acceleration != cameraInputAcceleration_ || smoothing != cameraInputSmoothing_) reset_camera_navigation();
    cameraInputAcceleration_ = acceleration;
    cameraInputSmoothing_ = smoothing;
    synthPanel_.set_keyboard_key_count(keyboard_key_count());
    chiptunePanel_.set_keyboard_key_count(keyboard_key_count());
    if (midiInput_) {
        midiInput_->set_channel_filter(midi_channel_filter_from_settings(workspace_.settings()));
        midiInput_->set_requested_port(midi_input_port());
    }
    if (midiOutput_) midiOutput_->set_requested_port(midi_output_port());
    try { refresh_midi_status(); } catch (...) {}
    viewportSettings_.showGrid = readBool("viewport.grid", viewportSettings_.showGrid);
    viewportSettings_.showAnchors = readBool("viewport.anchors", viewportSettings_.showAnchors);
    viewportSettings_.showCollision = readBool("viewport.collision", viewportSettings_.showCollision);
    viewportSettings_.showObjectBounds = readBool("viewport.bounds", viewportSettings_.showObjectBounds);
    viewportSettings_.xraySelection = readBool("viewport.xray_selection", viewportSettings_.xraySelection);
    viewportSettings_.maximumDrawVoxels = readInteger("viewport.max_voxels", viewportSettings_.maximumDrawVoxels);
    viewportSettings_.gridSpacingMeters = readFloat("viewport.grid_spacing", viewportSettings_.gridSpacingMeters);
    camera_.verticalFovRadians = readFloat("camera.field_of_view", 60.0F) * camera::kDegreesToRadians;
    camera_.nearPlane = readFloat("camera.near_plane", camera_.nearPlane);
    camera_.farPlane = std::max(camera_.nearPlane + 0.001F, readFloat("camera.far_plane", camera_.farPlane));
    camera_.projection = readString("camera.projection", "perspective") == "orthographic"
        ? EditorProjection::Orthographic : EditorProjection::Perspective;
    cameraDirector_.set_shake_scale(readFloat("camera.shake_scale", 1.0F) *
                                   readFloat("accessibility.camera_shake", 1.0F));
    cameraDirector_.set_reduced_motion(readBool("accessibility.reduced_motion", false));
    cinematicCameraPanel_.overlays().focusPlanes = readBool("camera.show_focus_planes", false);
    for (const auto& item : cameraDirector_.rigs()) {
        auto* rig = cameraDirector_.find_rig(item.id);
        if (!rig) continue;
        const auto previousFraming = rig->framing;
        const auto previousCollision = rig->collision;
        auto [it, inserted] = cameraRigSettingsBaselines_.try_emplace(item.id,
            CameraRigSettingsBaseline{rig->framing, rig->collision, rig->framing, rig->collision});
        (void)inserted;
        auto& baseline = it->second;
        const auto reconcile = [](auto& current, auto& authored, const auto& applied) {
            if (current != applied) authored = current; // preserve per-instance authoring
            current = authored;
        };
        reconcile(rig->framing.positionDampingSeconds, baseline.framing.positionDampingSeconds, baseline.appliedFraming.positionDampingSeconds);
        reconcile(rig->framing.aimDampingSeconds, baseline.framing.aimDampingSeconds, baseline.appliedFraming.aimDampingSeconds);
        reconcile(rig->framing.lookAheadSeconds, baseline.framing.lookAheadSeconds, baseline.appliedFraming.lookAheadSeconds);
        reconcile(rig->framing.deadZoneFraction, baseline.framing.deadZoneFraction, baseline.appliedFraming.deadZoneFraction);
        reconcile(rig->framing.softZoneFraction, baseline.framing.softZoneFraction, baseline.appliedFraming.softZoneFraction);
        reconcile(rig->collision.enabled, baseline.collision.enabled, baseline.appliedCollision.enabled);
        reconcile(rig->collision.probeRadiusMeters, baseline.collision.probeRadiusMeters, baseline.appliedCollision.probeRadiusMeters);
        reconcile(rig->collision.recoverySeconds, baseline.collision.recoverySeconds, baseline.appliedCollision.recoverySeconds);
        reconcile(rig->collision.preserveLineOfSight, baseline.collision.preserveLineOfSight, baseline.appliedCollision.preserveLineOfSight);
        configure_camera_rig(*rig, workspace_.settings(), false);
        if (!rig->framing.validate(&settingsError) || !rig->collision.validate(&settingsError)) {
            rig->framing = previousFraming;
            rig->collision = previousCollision;
            set_status("Camera settings were not activated for " + rig->name + ": " + settingsError, true);
        }
        baseline.appliedFraming = rig->framing;
        baseline.appliedCollision = rig->collision;
    }
    for (auto it = cameraRigSettingsBaselines_.begin(); it != cameraRigSettingsBaselines_.end();) {
        if (!cameraDirector_.find_rig(it->first)) it = cameraRigSettingsBaselines_.erase(it);
        else ++it;
    }
    audioSettingsPending_ = !apply_audio_settings_to_runtime();
    if (audioSettingsPending_) workspace_.log().add(EditorLogLevel::Warning,
        "Audio settings are pending until the callback drains its command queue.");
    camera_.physicalLens.enabled = readBool("camera.physical_lens", false);
    camera_.physicalLens.focalLengthMillimeters = readFloat("camera.focal_length_mm", 35.0F);
    const std::string sensor = readString("camera.sensor_preset", "full_frame");
    if (sensor == "super35") { camera_.physicalLens.sensorWidthMillimeters = 24.89F; camera_.physicalLens.sensorHeightMillimeters = 18.66F; }
    else if (sensor == "aps_c") { camera_.physicalLens.sensorWidthMillimeters = 23.6F; camera_.physicalLens.sensorHeightMillimeters = 15.7F; }
    else { camera_.physicalLens.sensorWidthMillimeters = 36.0F; camera_.physicalLens.sensorHeightMillimeters = 24.0F; }
    const std::string gate = readString("camera.gate_fit", "vertical");
    if (gate == "horizontal") camera_.physicalLens.gateFit = camera::CameraGateFit::Horizontal;
    else if (gate == "fill") camera_.physicalLens.gateFit = camera::CameraGateFit::Fill;
    else if (gate == "overscan") camera_.physicalLens.gateFit = camera::CameraGateFit::Overscan;
    else if (gate == "stretch") camera_.physicalLens.gateFit = camera::CameraGateFit::Stretch;
    else camera_.physicalLens.gateFit = camera::CameraGateFit::Vertical;
    camera_.physicalLens.apertureFStop = readFloat("camera.aperture", 2.8F);
    camera_.physicalLens.focusDistanceMeters = readFloat("camera.focus_distance", 10.0F);
    const std::string profile = shortcut_profile_name(readString("input.shortcut_profile", "dve"));
    std::string shortcutError;
    if (configuredShortcutProfile_ != profile) {
        clear_navigation_input();
        (void)workspace_.shortcuts().set_active_profile(profile, &shortcutError);
        configuredShortcutProfile_ = profile;
    }
    synchronize_menu_shortcuts();
    const auto previousMode = cameraMode_;
    const std::string mode = readString("camera.default_mode", "orbit");
    if (mode == "free") cameraMode_ = camera::CameraRigMode::FreeFly;
    else if (mode == "follow") cameraMode_ = camera::CameraRigMode::Follow;
    else if (mode == "third_person") cameraMode_ = camera::CameraRigMode::ThirdPerson;
    else if (mode == "first_person") cameraMode_ = camera::CameraRigMode::FirstPerson;
    else if (mode == "cinematic") cameraMode_ = camera::CameraRigMode::Cinematic;
    else cameraMode_ = camera::CameraRigMode::Orbit;
    if (previousMode != cameraMode_) clear_navigation_input();
    recompute_layout();
    refresh_menu_state();
    appliedSettingsRevision_ = workspace_.settings().revision();
}

void NativeEditorController::refresh_menu_state() noexcept {
    auto checked = [&](std::string_view id, bool value) { (void)workspace_.menus().set_checked(id, value); };
    auto enabled = [&](std::string_view id, bool value, std::string_view reason) {
        const MenuAction* action = std::as_const(workspace_.menus()).find(id);
        if (action && action->enabled == value &&
            (value ? action->disabledReason.empty() : action->disabledReason == reason)) return;
        (void)workspace_.menus().set_enabled(id, value, value ? std::string{} : std::string(reason));
    };
    checked("view.toggle_move_snap", workspace_.preferences().translateSnapEnabled);
    checked("view.toggle_angle_snap", workspace_.preferences().rotateSnapEnabled);
    checked("view.toggle_scale_snap", workspace_.preferences().scaleSnapEnabled);
    checked("view.toggle_absolute_grid", workspace_.preferences().absoluteGridSnap);
    checked("transform.align_surface", alignToSurfaceNormal_);
    for (const int keys : kPianoKeyboardSizes)
        checked("view.keyboard_keys_" + std::to_string(keys), keyboard_key_count() == keys);
    const bool hasSelection = workspace_.selection_count() > 0U;
    const bool singleSelection = workspace_.selection_count() == 1U;
    const bool playActive = playSession_.active();
    enabled("edit.undo", workspace_.commands().can_undo(), "There is no authored edit to undo.");
    enabled("edit.redo", workspace_.commands().can_redo(), "There is no authored edit to redo.");
    for (std::string_view id : {"edit.cut", "edit.copy", "edit.delete", "edit.duplicate", "edit.group"})
        enabled(id, hasSelection, "Select one or more scene objects first.");
    if (selectionDomain_ == SelectionDomain::Voxels) {
        // In the Voxels domain Cut, Copy and Delete act on voxels only (never the whole object).
        for (std::string_view id : {"edit.cut", "edit.copy", "edit.delete"})
            enabled(id, !voxelSelection_.empty(), "No voxels selected: click or drag on a voxel object (M returns to objects).");
    }
    enabled("edit.rename", singleSelection, "Select exactly one scene object to rename.");
    enabled("file.export_print_stl", singleSelection, "Select exactly one voxel object to export for printing.");
    {
        // Half-model mirroring: the primary polygon mesh (or the mesh being edited in the polygon domain).
        const bool components = selectionDomain_ == SelectionDomain::Polygons;
        const auto meshId = components ? std::optional(polygonSelection_.object) : workspace_.selected_object();
        const EditorObject* mesh = meshId ? workspace_.document().find_object(*meshId) : nullptr;
        const bool meshReady = mesh && mesh->polygon && !playActive;
        const bool facesReady = meshReady && components && !polygonSelection_.components.empty();
        const auto liveAxis = meshId ? live_mirror_axis(*meshId) : std::nullopt;
        for (const char axis : {'x', 'y', 'z'}) {
            const int index = axis - 'x';
            enabled(std::string("mirror.mesh_") + axis, meshReady, "Select a polygon mesh or subdivision cage.");
            enabled(std::string("mirror.faces_") + axis, facesReady, "Select faces of a mesh in the polygon selection mode.");
            enabled(std::string("mirror.live_") + axis, meshReady, "Select a polygon mesh or subdivision cage.");
            checked(std::string("mirror.live_") + axis, liveAxis == index);
        }
        enabled("mirror.apply_live", meshReady && liveAxis.has_value(), "The selected mesh has no live mirror.");
    }
    for (std::string_view id : {"edit.mirror_x", "edit.mirror_y", "edit.mirror_z", "edit.mirror_copy_x", "edit.mirror_copy_y", "edit.mirror_copy_z"})
        enabled(id, hasSelection && !playActive && selectionDomain_ == SelectionDomain::Objects,
                playActive ? "Stop Play first." : selectionDomain_ != SelectionDomain::Objects ? "Return to object selection first."
                                                                                                : "Select the objects to mirror first.");
    enabled("edit.ungroup", hasSelection, "Select a grouped object first.");
    enabled("edit.duplicate_special", (hasSelection || duplicateSpecial_.active) && !playActive &&
                selectionDomain_ == SelectionDomain::Objects,
            playActive ? "Stop Play first." : "Select the objects to copy first.");
    {
        bool canOpen = false, canClose = false;
        for (const EditorObjectId id : workspace_.selected_objects()) {
            const EditorObjectId target = is_group(id) ? id : viewport_selection_target(id);
            if (is_group(target) && !openGroups_.contains(target)) canOpen = true;
            if (openGroups_.contains(id)) canClose = true;
            for (const EditorObject* object = workspace_.document().find_object(id); !canClose && object && object->parent;) {
                if (openGroups_.contains(*object->parent)) canClose = true;
                object = workspace_.document().find_object(*object->parent);
            }
        }
        if (!hasSelection && !openGroups_.empty()) canClose = true;
        const auto active = workspace_.selected_object();
        const bool singleGroup = singleSelection && active && is_group(*active);
        const EditorObject* activeGroup = singleGroup ? workspace_.document().find_object(*active) : nullptr;
        const bool pivotFixed = activeGroup && imported_group(*activeGroup);
        enabled("edit.open_group", canOpen && !playActive, "Select a closed group (or double-click a piece in one).");
        enabled("edit.close_group", canClose && !playActive, "Select something inside an open group first.");
        enabled("edit.close_all_groups", !openGroups_.empty(), "No groups are open.");
        enabled("edit.edit_pivot", ((singleGroup && !pivotFixed) || pivotEditing_) && !playActive,
                pivotFixed ? kImportedPivotFixed : std::string_view("Select one group to edit its pivot."));
        checked("edit.edit_pivot", pivotEditing_);
        for (std::string_view id : {"edit.pivot_bottom_center", "edit.pivot_center"})
            enabled(id, singleGroup && !pivotFixed && !playActive,
                    pivotFixed ? kImportedPivotFixed : std::string_view("Select one group first."));
    }
    const auto hierarchyPrimary = workspace_.selected_object();
    const auto* hierarchyObject = hierarchyPrimary ? workspace_.document().find_object(*hierarchyPrimary) : nullptr;
    const bool hierarchySelectionReady = !polygonPreviewBefore_ && !mesh_authoring_dragging() &&
        !gizmoDragging_ && !longJob_ && !importOptions_.open;
    const std::string_view hierarchyPreviewReason = "Apply or cancel the active preview first.";
    enabled("select.parent", hierarchySelectionReady && singleSelection && hierarchyObject && hierarchyObject->parent.has_value(),
            hierarchySelectionReady ? "Select one object with a parent." : hierarchyPreviewReason);
    enabled("select.children", hierarchySelectionReady && hasSelection,
            hierarchySelectionReady ? "Select a parent object." : hierarchyPreviewReason);
    enabled("select.siblings", hierarchySelectionReady && singleSelection,
            hierarchySelectionReady ? "Select one scene object." : hierarchyPreviewReason);
    {
        const std::string voxelSizeReason = voxel_size_scale_disabled_reason();
        for (std::string_view id : {"transform.scale_voxel_size_up", "transform.scale_voxel_size_down"})
            enabled(id, voxelSizeReason.empty(), voxelSizeReason);
    }
    enabled("edit.paste", !clipboard_.empty() || (voxelClipboard_ && !voxelClipboard_->cells.empty()),
            "Copy or cut an object (or voxels) before pasting.");
    enabled("asset.open", assetBrowserState_.selectedId.has_value(), "Select an asset in the Assets panel first.");
    enabled("asset.rename", assetBrowserState_.selectedId.has_value(), "Select an asset in the Assets panel first.");
    enabled("asset.thumbnail", assetBrowserState_.selectedId.has_value(), "Select an asset before regenerating its thumbnail.");
    enabled("asset.instantiate_prefab", assetBrowserState_.selectedId.has_value(), "Select a prefab asset first.");
    enabled("physics.play", !playActive, "A Play or Simulate session is already running.");
    enabled("physics.simulate", !playActive, "A Play or Simulate session is already running.");
    enabled("physics.stop", playActive || spriteLevelPlaying_, "No Play or Simulate session is running.");
    enabled("sprite.save", spriteAuthoringPanel_.open(), "Open a sprite asset before saving it.");
    enabled("sprite.reimport", spriteAuthoringPanel_.open(), "Open a sprite asset before reimporting its source.");
    enabled("sprite.slice_grid", spriteAuthoringPanel_.open(), "Open a sprite texture before slicing it.");
    enabled("sprite.repack", spriteAuthoringPanel_.open(), "Open a sprite asset before repacking its atlas.");
    enabled("text3d.edit_selected", singleSelection, "Select one 3D text object first.");
    enabled("text3d.commit", text3dAuthoring_.active(), "No 3D text authoring session is active.");
    {
        enabled("create.scatter", scatter_disabled_reason().empty(), scatter_disabled_reason());
        enabled("scatter.commit", scatterActive_ && scatterPlan_.error.empty() && !scatterPlan_.samples.empty(),
                scatterActive_ ? "The scatter preview found no spots on the surface." : "No scatter preview is open.");
        enabled("scatter.cancel", scatterActive_, "No scatter preview is open.");
        enabled("scatter.brush", workspace_.mode() == EditorMode::Edit && !playSession_.active(),
                "Stop Play or Simulate before painting.");
        enabled("scatter.set_sources", workspace_.mode() == EditorMode::Edit && !playSession_.active() &&
                    (workspace_.selection_count() > 0 || assetBrowserState_.selectedId.has_value()),
                "Select the objects (or a prefab) to scatter first.");
        enabled("scatter.clear_sources", scatter_sources_pinned(), "No scatter sources are set.");
        const auto sliceReason = voxel_slice_disabled_reason();
        enabled("voxel.slice", sliceReason.empty(), sliceReason);
        enabled("voxel.slice_commit", voxelSlice_.active() && voxelSlice_.blocked_reason(workspace_.document()).empty(), voxelSlice_.blocked_reason(workspace_.document()));
        enabled("voxel.slice_cancel", voxelSlice_.active(), "No Slice preview is open.");
        {
            const EditorAssetRecord* asset = assetBrowserState_.selectedId
                ? std::as_const(assetDatabase_).find(*assetBrowserState_.selectedId) : nullptr;
            std::string extension = asset ? asset->relativePath.extension().string() : std::string();
            for (char& ch : extension) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            const bool model = extension == ".obj" || extension == ".gltf" || extension == ".glb";
            enabled("polygon.import", (model || extension == ".dmesh") && !playActive,
                    playActive ? "Stop Play or Simulate before importing."
                               : "Select an OBJ, glTF, GLB or DMESH model in the Assets panel first.");
            enabled("polygon.import_exact", asset && is_exact_cad_source(asset->relativePath) && !playActive,
                    playActive ? "Stop Play or Simulate before importing."
                               : "Select a STEP, BREP or OpenUSD B-rep (.usda) file in the Assets panel first.");
            for (std::string_view id : {"file.import"})
                enabled(id, model && !playActive, playActive ? "Stop Play or Simulate before importing."
                                                             : "Select an OBJ, glTF or GLB model in the Assets panel first.");
        }
        for (std::string_view id : {"file.open_scene", "file.open_project", "file.save_as"})
            enabled(id, !playActive && workspace_.mode() == EditorMode::Edit, "Stop Play or Simulate first.");
        enabled("hierarchy.reveal_selection", workspace_.selected_object().has_value(), "Select an object first.");
        for (std::string_view id : {"hierarchy.expand_all", "hierarchy.collapse_all"})
            enabled(id, hierarchyFilter_.empty(), "Clear the hierarchy filter first.");
        enabled("voxel.fill", !playActive && workspace_.mode() == EditorMode::Edit, "Stop Play or Simulate before filling.");
        enabled("view.drop_to_surface", hasSelection && !playActive && workspace_.mode() == EditorMode::Edit,
                playActive || workspace_.mode() != EditorMode::Edit ? "Stop Play or Simulate before moving objects."
                                                                    : "Select the objects to drop first.");
        enabled("view.follow_selection", followSelection_ || hasSelection, "Select something to follow first.");
        // Not implemented in this editor build: disabled with the reason, so the menu and the
        // Command Center say so before the command is tried.
        enabled("voxel.rebuild_brickmaps", false, kRebuildBrickmapsUnavailable);
        enabled("audio.import", false, kAudioImportUnavailable);
        for (std::string_view id : {"create.polygon", "polygon.create_plane"})
            enabled(id, !playActive && workspace_.mode() == EditorMode::Edit, kCreatePolygonBlocked);
        const std::string joinReason = voxel_join_disabled_reason();
        enabled("voxel.join", joinReason.empty(), joinReason);
        const std::string joinBlocked = voxelJoin_.blocked_reason(workspace_.document());
        enabled("voxel.join_commit", voxelJoin_.active() && joinBlocked.empty(), joinBlocked);
        enabled("voxel.join_cancel", voxelJoin_.active(), "No Join preview is open.");
        const std::string separateReason = separate_islands_disabled_reason();
        enabled("voxel.separate_islands", separateReason.empty(), separateReason);
        const auto primary = workspace_.selected_object();
        const auto* conversionObject = primary ? workspace_.document().find_object(*primary) : nullptr;
        const bool conversionReady = workspace_.mode() == EditorMode::Edit && !playSession_.active() && !longJob_ &&
            !voxel_authoring_active() && !gizmoDragging_ && !voxelStrokeActive_ && voxelMoveAxis_ == 0 &&
            !marquee_.active && !mesh_authoring_dragging() && !polygonPreviewBefore_ &&
            workspace_.selection_count() == 1 && conversionObject &&
            conversionObject->flags.visible && !conversionObject->flags.locked && !is_isolated_out(conversionObject->id);
        enabled("polygon.convert_to_voxels",conversionReady && conversionObject->polygon.has_value(),
                "Select one visible unlocked polygon mesh and finish any preview.");
        enabled("voxel.convert_to_polygon",conversionReady && conversionObject->voxels && !conversionObject->polygon &&
                !conversionObject->text3d && !conversionObject->gaborVolume,
                "Select one visible unlocked voxel object and finish any preview.");
        enabled("voxel.authoring_apply",voxel_authoring_preview_ready() && !longJob_,"Update the preview before applying.");
        enabled("voxel.authoring_cancel",voxel_authoring_active() && !longJob_,"No voxel/conversion preview is open.");
        for (auto id : {"voxel.select_cells","voxel.select_connected"})
            enabled(id,workspace_.mode() == EditorMode::Edit && !playSession_.active() && !longJob_,
                    "Stop Play and finish background work before changing selection mode.");
        checked("voxel.select_cells",voxelClickSelection_ == VoxelClickSelection::Cell);
        checked("voxel.select_connected",voxelClickSelection_ == VoxelClickSelection::ConnectedPiece);
        const auto regionReason = voxel_region_disabled_reason();
        for (std::string_view id : {
            "voxel.region_extrude_x_positive",
            "voxel.region_extrude_x_negative",
            "voxel.region_extrude_y_positive",
            "voxel.region_extrude_y_negative",
            "voxel.region_extrude_z_positive",
            "voxel.region_extrude_z_negative",
            "voxel.region_rotate_x_positive",
            "voxel.region_rotate_x_negative",
            "voxel.region_rotate_y_positive",
            "voxel.region_rotate_y_negative",
            "voxel.region_rotate_z_positive",
            "voxel.region_rotate_z_negative",
            "voxel.region_scale_2",
            "voxel.region_scale_3",
            "voxel.region_scale_4"})
            enabled(id, regionReason.empty(), regionReason);
        enabled("voxel.selection_brush", workspace_.mode() == EditorMode::Edit && !playSession_.active(),
                "Stop Play before selecting voxels.");
        checked("voxel.selection_brush", selectionDomain_ == SelectionDomain::Voxels &&
                activeTool_ == EditorToolId::Select && voxelSelectionBrushEnabled_);
        const bool voxelsSelected = !voxelSelection_.empty();
        for (std::string_view id : {"voxel.select_grow", "voxel.select_shrink", "voxel.select_none", "voxel.extract_selection"})
            enabled(id, voxelsSelected, "Select voxels first: press M, then click or drag on a voxel object.");
        enabled("voxel.select_invert", voxelSelectionObject_ != 0, "Press M and click a voxel object first.");
        {
            EditorObjectId target = voxelSelectionObject_;
            if (!workspace_.document().find_object(target) && workspace_.selected_object()) target = *workspace_.selected_object();
            const EditorObject* object = workspace_.document().find_object(target);
            enabled("voxel.select_all", object && object->voxels && !object->polygon && !object->text3d && !object->gaborVolume,
                    "Select a voxel object first.");
        }
        const std::string booleanReason = voxel_boolean_disabled_reason();
        for (std::string_view id : {"voxel.boolean_union", "voxel.boolean_difference", "voxel.boolean_intersection"})
            enabled(id, booleanReason.empty(), booleanReason);
        enabled("voxel.boolean_commit", voxelBoolean_.can_commit(), voxelBoolean_.commit_blocked_reason());
        enabled("voxel.boolean_swap", voxelBoolean_.active(),
                "No Boolean preview is open. Select voxel objects and choose Tools > Voxel Boolean.");
        enabled("voxel.boolean_cancel", voxelBoolean_.active(),
                "No Boolean preview is open. Select voxel objects and choose Tools > Voxel Boolean.");
        checked("voxel.boolean_operands_hide", voxelBooleanPolicy_ == VoxelBooleanOperandPolicy::Hide);
        checked("voxel.boolean_operands_delete", voxelBooleanPolicy_ == VoxelBooleanOperandPolicy::Delete);
        checked("voxel.boolean_operands_keep", voxelBooleanPolicy_ == VoxelBooleanOperandPolicy::Keep);
    }
    {
        const std::string reimportReason = reimport_disabled_reason();
        enabled("file.reimport", reimportReason.empty(), reimportReason);
        // Polygon mesh and material editing commands that only had shortcuts (#101): listed in
        // their menus (and so the Command Center) as disabled, with the reason, before use.
        for (const auto& [id, reason] : editor_context_only_commands()) enabled(id, false, reason);
        if (longJob_) {
            const std::string busy = long_job_busy_reason();
            for (std::string_view id : {"file.import", "polygon.import", "polygon.import_exact", "file.reimport", "voxel.fill", "voxel.rebuild_collision",
                                        "build.validate", "file.open_scene", "file.open_project", "file.save_as"})
                enabled(id, false, busy);
        }
    }
    {
        const auto primary=workspace_.selected_object();
        const auto* object=primary?workspace_.document().find_object(*primary):nullptr;
        const bool mesh=object && object->polygon && object->flags.visible && !object->flags.locked &&
            !is_isolated_out(object->id) && !playActive && workspace_.mode()==EditorMode::Edit && !longJob_;
        const bool components=mesh && selectionDomain_==SelectionDomain::Polygons && !polygonSelection_.components.empty();
        const bool preview=polygonPreviewBefore_.has_value();
        for (auto id:{"polygon.vertex_mode","polygon.edge_mode","polygon.face_mode","polygon.object_mode","polygon.component_toggle"})
            enabled(id,mesh && !preview,"Select an unlocked polygon mesh in Edit mode first.");
        for (auto id:{"polygon.extrude","polygon.inset","polygon.bevel","polygon.uv_tools","polygon.flip_normals","polygon.extract","polygon.assign_material"})
            enabled(id,components && polygonSelection_.mode==PolygonSelectionMode::Face && !preview,"Select polygon triangle faces first.");
        for (auto id:{"polygon.select_shell","polygon.select_grow","polygon.select_shrink"})
            enabled(id,components && !preview,"Select polygon components first.");
        for(auto id:{"polygon.weld","polygon.dissolve_vertices"})
            enabled(id,components && workspace_.selection_count()==1 && polygonSelection_.object==object->id &&
                polygonSelection_.mode==PolygonSelectionMode::Vertex && (id!=std::string_view{"polygon.weld"} || polygonSelection_.components.size()>1) && !preview && !gizmoDragging_ &&
                !mesh_authoring_dragging() && !voxel_authoring_active(),
                "Select vertices on one visible unlocked mesh and finish the active edit first.");
        for (auto id:{"polygon.select_loop","polygon.select_ring"})
            enabled(id,components && object->subdivision && polygonSelection_.object==object->id &&
                polygonSelection_.mode==PolygonSelectionMode::Edge && !preview && !gizmoDragging_ &&
                !mesh_authoring_dragging() && !voxel_authoring_active(),
                "Select subdivision control-cage edges and finish the active edit first.");
        enabled("polygon.recalculate_normals",mesh && selectionDomain_==SelectionDomain::Polygons && !preview,"Enter a polygon component mode first.");
        enabled("polygon.loop_cut",components && object->subdivision && polygonSelection_.object==object->id &&
            polygonSelection_.mode==PolygonSelectionMode::Edge && polygonSelection_.components.size()==1 &&
            !preview && !gizmoDragging_ && !mesh_authoring_dragging() && !voxel_authoring_active(),
            "Select one subdivision cage edge and finish the active edit first.");
        enabled("polygon.dissolve_edges",components && object->subdivision && polygonSelection_.object==object->id &&
            polygonSelection_.mode==PolygonSelectionMode::Edge && !preview && !gizmoDragging_ &&
            !mesh_authoring_dragging() && !voxel_authoring_active(),
            "Select subdivision cage edges and finish the active edit first.");
        {
            const bool ready=mesh && workspace_.selection_count()==1 && !preview && !gizmoDragging_ &&
                !mesh_authoring_dragging() && !voxel_authoring_active();
            bool unused=false;
            if (ready) {
                std::vector<bool> used(object->polygon->vertices.size());
                for (auto index : object->polygon->indices) if (index<used.size()) used[index]=true;
                unused=std::find(used.begin(),used.end(),false)!=used.end();
            }
            enabled("polygon.clean_unused",ready && unused,ready?"No unused vertices to remove.":
                "Select one visible unlocked mesh and finish the active edit first.");
        }
        for (auto id:{"polygon.select_invert","polygon.select_none"}) enabled(id,mesh && selectionDomain_==SelectionDomain::Polygons && !preview,"Enter polygon component mode first.");
        enabled("polygon.join",mesh && workspace_.selection_count()>1 && !preview,"Select at least two polygon meshes.");
        enabled("polygon.slice",mesh && !preview,"Select a polygon mesh first.");
        enabled("polygon.separate",mesh && !preview,"Select a polygon mesh first.");
        enabled("polygon.preview_apply",preview && polygonPreviewValid_,"No valid polygon preview to apply.");
        enabled("polygon.preview_cancel",preview,"No polygon preview to cancel.");
        if (selectionDomain_==SelectionDomain::Polygons) {
            for (auto id:{"edit.cut","edit.copy","edit.delete","edit.duplicate"}) enabled(id,components && !preview,"Select polygon components first.");
            for (auto id:{"edit.cut","edit.copy"}) enabled(id,components && polygonSelection_.mode==PolygonSelectionMode::Face && !preview,"Select polygon faces to copy/cut.");
        }
        enabled("edit.paste",!clipboard_.empty() || (voxelClipboard_ && !voxelClipboard_->cells.empty()) || polygonClipboard_.has_value(),"Copy geometry before pasting.");
        if (preview) for (auto id:{"edit.undo","edit.redo","edit.delete","edit.cut","edit.paste","edit.duplicate","physics.play","physics.simulate"})
            enabled(id,false,"Apply or cancel the polygon preview first.");
        checked("polygon.object_mode",selectionDomain_==SelectionDomain::Objects);
        checked("polygon.vertex_mode",selectionDomain_==SelectionDomain::Polygons && polygonSelection_.mode==PolygonSelectionMode::Vertex);
        checked("polygon.edge_mode",selectionDomain_==SelectionDomain::Polygons && polygonSelection_.mode==PolygonSelectionMode::Edge);
        checked("polygon.face_mode",selectionDomain_==SelectionDomain::Polygons && polygonSelection_.mode==PolygonSelectionMode::Face);
    }
    refresh_mesh_authoring_menu_state();
    refresh_subdivision_menu_state();
    enabled("text3d.cancel", text3dAuthoring_.active(), "No 3D text authoring session is active.");
    // Cook, Package and Documentation have no handler in this editor build: say so up front.
    enabled("build.cook", false, kCookUnavailable);
    enabled("build.package", false, kPackageUnavailable);
    enabled("help.docs", false, kDocsUnavailable);
    enabled("camera.pilot_selected", selectedCameraRig_.has_value(), "Select or create a camera rig first.");
    enabled("camera.align_to_view", selectedCameraRig_.has_value(), "Select or create a camera rig first.");
    enabled("camera.keyframe_profile", selectedCameraRig_.has_value(), "Select or create a camera rig first.");
    enabled("camera.paste_profile", cinematicCameraPanel_.can_paste(), "Copy a cinematic profile first.");
    checked("view.grid", viewportSettings_.showGrid);
    checked("view.isolate_selection", !viewportSettings_.isolatedObjects.empty());
    checked("view.collision", viewportSettings_.showCollision);
    checked("view.anchors", viewportSettings_.showAnchors);
    checked("view.bounds", viewportSettings_.showObjectBounds);
    checked("view.xray", viewportSettings_.xraySelection);
    checked("view.follow_selection", followSelection_);
    checked("select.mask_locked", selectionMask_.locked);
    checked("select.mask_helpers", selectionMask_.helpers);
    checked("select.mask_prefabs", selectionMask_.prefabs);
    checked("select.shape_box", selectionShape_ == SelectionShape::Box);
    checked("select.shape_lasso", selectionShape_ == SelectionShape::Lasso);
    checked("select.shape_paint", selectionShape_ == SelectionShape::Paint);
    checked("select.domain_objects", selectionDomain_ == SelectionDomain::Objects);
    checked("voxel.brush_shape_cube", voxelBrushShape_ == BrushShape::Cube);
    checked("voxel.brush_shape_sphere", voxelBrushShape_ == BrushShape::Sphere);
    checked("select.domain_voxels", selectionDomain_ == SelectionDomain::Voxels);
    {
        const SettingValue shadingValue = workspace_.settings().value("viewport.shading_mode");
        const std::string shading = std::get_if<std::string>(&shadingValue) ? std::get<std::string>(shadingValue) : std::string("lit");
        for (const auto& [id, mode] : std::array<std::pair<std::string_view, std::string_view>, 8>{{
                 {"view.lit", "lit"}, {"view.unlit", "unlit"}, {"view.wireframe", "wireframe"}, {"view.material_ids", "material_ids"},
                 {"view.normals", "normals"}, {"view.lighting_only", "lighting_only"}, {"view.overdraw", "overdraw"},
                 {"view.voxel_debug", "voxel_debug"}}})
            checked(id, shading == mode);
    }
    const auto settingBool = [&](std::string_view id, bool fallback) {
        const SettingValue value = workspace_.settings().value(id);
        if (const auto* item = std::get_if<bool>(&value)) return *item;
        return fallback;
    };
    checked("view.statistics", settingBool("viewport.statistics", false));
    checked("view.safe_frames", settingBool("viewport.safe_frames", false));
    checked("view.advanced_menus", showAdvancedMenus_);
    checked("camera.mode_free", cameraMode_ == camera::CameraRigMode::FreeFly);
    checked("camera.mode_orbit", cameraMode_ == camera::CameraRigMode::Orbit);
    checked("camera.mode_follow", cameraMode_ == camera::CameraRigMode::Follow);
    checked("camera.mode_third_person", cameraMode_ == camera::CameraRigMode::ThirdPerson);
    checked("camera.mode_first_person", cameraMode_ == camera::CameraRigMode::FirstPerson);
    checked("camera.mode_cinematic", cameraMode_ == camera::CameraRigMode::Cinematic);
    checked("camera.projection_perspective", camera_.projection == EditorProjection::Perspective);
    checked("camera.projection_orthographic", camera_.projection == EditorProjection::Orthographic);
    checked("camera.physical_lens", camera_.physicalLens.enabled);
    checked("camera.collision", settingBool("camera.collision", true));
    checked("camera.preview_selected", settingBool("camera.preview_selected", true));
    checked("camera.show_frustum", settingBool("camera.show_frustum", true));
    checked("camera.preview_shakes", settingBool("camera.allow_shakes_in_editor", true));
    checked("window.camera_preview", settingBool("camera.preview_selected", true));
    checked("window.cinematic_camera", cinematicCameraPanel_.is_open());
    checked("window.camera_sequencer", cinematicCameraPanel_.sequencer_open());
    checked("camera.inspector", cinematicCameraPanel_.is_open());
    checked("camera.sequencer", cinematicCameraPanel_.sequencer_open());
    checked("camera.lock_viewport", settingBool("camera.lock_viewport_to_selected", false));
    checked("camera.preview_effects", settingBool("camera.preview_cinematic_effects", true));
    checked("camera.overlay_focus", cinematicCameraPanel_.overlays().focusPlanes);
    checked("camera.overlay_split_diopter", cinematicCameraPanel_.overlays().splitDiopter);
    checked("camera.overlay_safe_frames", cinematicCameraPanel_.overlays().safeFrames);
    checked("camera.overlay_aspect_mattes", cinematicCameraPanel_.overlays().aspectMattes);
    checked("camera.overlay_motion_vectors", cinematicCameraPanel_.overlays().motionVectors);
    checked("camera.overlay_exposure", cinematicCameraPanel_.overlays().exposurePreview);
    checked("camera.compare_graded", cinematicCameraPanel_.overlays().compareUngraded);
    checked("camera.scope_instance", cinematicCameraPanel_.scope() == CinematicCameraScope::CameraInstance);
    checked("camera.scope_shot", cinematicCameraPanel_.scope() == CinematicCameraScope::ShotOverride);
    checked("camera.scope_project", cinematicCameraPanel_.scope() == CinematicCameraScope::ProjectDefault);
    checked("camera.scope_preview", cinematicCameraPanel_.scope() == CinematicCameraScope::ViewportPreview);
    checked("window.toggle_live_mcp", liveMcpHost_ != nullptr);
    checked("window.toggle_control_rig", controlRigPanel_.open());
    checked("window.toggle_hierarchy", workspace_.panel_visible(PanelId::SceneHierarchy));
    checked("window.toggle_inspector", workspace_.panel_visible(PanelId::Inspector));
    checked("window.toggle_assets", workspace_.panel_visible(PanelId::Assets));
    checked("window.toggle_synth", synthPanel_.open());
    checked("window.toggle_audio", audioPanel_.open());
    checked("window.toggle_audio_event", audioEventPanel_.open());
    checked("window.toggle_ai_assistant", workspace_.panel_visible(PanelId::Assistant));
    checked("window.toggle_chiptune", chiptunePanel_.open());
    checked("sprite.tile_world", tileWorldEditorOpen_);
    checked("sprite.diagnostics", spriteDiagnosticsOpen_);
    checked("render.diagnostics3d", render3DDiagnosticsOpen_);
    checked("sprite.graph", spriteAnimationGraphOpen_);
    checked("sprite.pixel_art", spritePixelArtOpen_);
    checked("sprite.rig2d", spriteRig2DOpen_);
    checked("sprite.editor", spriteAuthoringPanel_.open());
    checked("render.gabor.enabled", settingBool("render.gabor.enabled", true));
    const auto settingString = [&](std::string_view id, std::string fallback) {
        const SettingValue value = workspace_.settings().value(id);
        if (const auto* item = std::get_if<std::string>(&value)) return *item;
        return fallback;
    };
    const std::string gaborMode = settingString("render.gabor.mode", "emission_absorption");
    checked("render.gabor.mode_absorption", gaborMode == "absorption");
    checked("render.gabor.mode_emission_absorption", gaborMode == "emission_absorption");
    checked("render.gabor.mode_scattering", gaborMode == "scattering");
    const std::string gaborQuality = settingString("render.gabor.quality", "medium");
    checked("render.gabor.quality_low", gaborQuality == "low");
    checked("render.gabor.quality_medium", gaborQuality == "medium");
    checked("render.gabor.quality_high", gaborQuality == "high");
    checked("render.gabor.quality_cinematic", gaborQuality == "cinematic");
    checked("render.gabor.continuous_lod", settingBool("render.gabor.continuous_lod", true));
    checked("render.gabor.temporal", settingBool("render.gabor.temporal_accumulation", true));
    checked("render.gabor.shadows", settingBool("render.gabor.cast_shadows", true));
    checked("view.perspective", camera_.projection == EditorProjection::Perspective);
    synchronize_menu_shortcuts();
}

void NativeEditorController::handle_settings_key(std::string_view normalized, bool control, bool shift, bool alt) {
    (void)alt;
    const auto rows = settings_rows();
    const auto selectedDefinition = [&]() -> const SettingDefinition* {
        return settingsPanel_.selectedRow < rows.size() ? rows[settingsPanel_.selectedRow] : nullptr;
    };

    if (normalized == "escape") {
        if (settingsPanel_.valueEditing) settingsPanel_.cancel_value_edit();
        else close_settings(false);
        return;
    }
    if (settingsPanel_.valueEditing) {
        if (normalized == "enter" || normalized == "return") {
            std::string error;
            if (!settingsPanel_.commit_value_edit(workspace_.settings(), &error))
                settingsPanel_.status = error;
            return;
        }
        if (normalized == "backspace") {
            settingsPanel_.backspace_value_text();
            return;
        }
        return;
    }
    if (control && normalized == "r") {
        std::string error;
        if (shift) {
            const std::size_t count = settingsPanel_.reset_category(
                workspace_.settings(), settingsPanel_.selectedCategory);
            settingsPanel_.status = count == 0U ? "Nothing to reset" :
                std::to_string(count) + " options will inherit defaults";
        } else if (const SettingDefinition* definition = selectedDefinition()) {
            if (!settingsPanel_.reset(workspace_.settings(), definition->id, &error))
                settingsPanel_.status = error;
        }
        return;
    }
    if (normalized == "f3") {
        settingsPanel_.changedOnly = !settingsPanel_.changedOnly;
        settingsPanel_.selectedRow = 0U;
        return;
    }
    if (normalized == "enter" || normalized == "return" || normalized == "f2") {
        const SettingDefinition* definition = selectedDefinition();
        if (!definition) { close_settings(true); return; }
        std::string error;
        if (settingsPanel_.has_choices(*definition)) {
            if (!settingsPanel_.cycle(workspace_.settings(), definition->id, 1, &error))
                settingsPanel_.status = error;
        } else if (!settingsPanel_.begin_value_edit(workspace_.settings(), definition->id, &error)) {
            settingsPanel_.status = error;
        }
        return;
    }
    if (normalized == "up") {
        if (settingsPanel_.selectedRow > 0U) --settingsPanel_.selectedRow;
        return;
    }
    if (normalized == "down") {
        if (!rows.empty()) settingsPanel_.selectedRow =
            std::min(settingsPanel_.selectedRow + 1U, rows.size() - 1U);
        return;
    }
    if (normalized == "left" || normalized == "right") {
        if (const SettingDefinition* definition = selectedDefinition()) {
            std::string error;
            if (!settingsPanel_.cycle(workspace_.settings(), definition->id,
                                      normalized == "right" ? 1 : -1, &error))
                settingsPanel_.status = error;
        }
        return;
    }
    if (normalized == "tab") {
        if (control) {
            const int direction = shift ? -1 : 1;
            int index = settingsPanel_.scope == SettingScope::User ? 0
                : settingsPanel_.scope == SettingScope::Project ? 1 : 2;
            index = (index + direction + 3) % 3;
            if (settingsPanel_.dirty) {
                settingsPanel_.status = "Apply or discard changes before changing scope";
            } else {
                settingsPanel_.open_for(index == 0 ? SettingScope::User
                    : index == 1 ? SettingScope::Project : SettingScope::Session,
                    settingsPanel_.selectedCategory);
            }
            return;
        }
        const auto categories = settings_categories();
        const auto it = std::find(categories.begin(), categories.end(), settingsPanel_.selectedCategory);
        std::size_t index = it == categories.end() ? 0U :
            static_cast<std::size_t>(std::distance(categories.begin(), it));
        if (!categories.empty()) {
            index = shift ? (index + categories.size() - 1U) % categories.size()
                          : (index + 1U) % categories.size();
            settingsPanel_.selectedCategory = categories[index];
            settingsPanel_.searchQuery.clear();
            settingsPanel_.selectedRow = 0U;
        }
        return;
    }
    if (normalized == "backspace") {
        if (!settingsPanel_.searchQuery.empty()) settingsPanel_.searchQuery.pop_back();
        settingsPanel_.selectedRow = 0U;
    }
}

void NativeEditorController::handle_shortcut_editor_key(std::string_view normalized, bool control, bool shift, bool alt) {
    const auto rows = shortcut_rows();
    if (shortcutPanel_.capturing) {
        if (normalized == "escape") {
            shortcutPanel_.capturing = false;
            shortcutPanel_.status = "Binding capture cancelled";
            return;
        }
        ShortcutGesture gesture = keyboard_shortcut(std::string(normalized), control, shift, alt);
        std::string error;
        const SettingValue warningValue = workspace_.settings().value("input.warn_shortcut_conflicts");
        const bool overrideConflicts = std::get_if<bool>(&warningValue) && !std::get<bool>(warningValue);
        if (workspace_.shortcuts().set_binding(workspace_.shortcuts().active_profile(),
                                               shortcutPanel_.captureActionId, shortcutPanel_.captureContext,
                                               shortcutPanel_.captureSlot, gesture, overrideConflicts, &error)) {
            shortcutPanel_.capturing = false;
            shortcutPanel_.status = "Assigned " + format_shortcut_gesture(gesture);
            synchronize_menu_shortcuts();
        } else shortcutPanel_.status = error;
        return;
    }
    if (normalized == "escape") { close_shortcut_editor(); return; }
    if (normalized == "up") { if (shortcutPanel_.selectedRow > 0U) --shortcutPanel_.selectedRow; return; }
    if (normalized == "down") {
        if (!rows.empty()) shortcutPanel_.selectedRow = std::min(shortcutPanel_.selectedRow + 1U, rows.size() - 1U);
        return;
    }
    if (normalized == "left" || normalized == "right") {
        const auto profiles = workspace_.shortcuts().profile_names();
        const auto found = std::find(profiles.begin(), profiles.end(), std::string(workspace_.shortcuts().active_profile()));
        std::size_t index = found == profiles.end() ? 0U : static_cast<std::size_t>(std::distance(profiles.begin(), found));
        if (!profiles.empty()) {
            if (normalized == "right") index = (index + 1U) % profiles.size();
            else index = (index + profiles.size() - 1U) % profiles.size();
            std::string error;
            if (workspace_.shortcuts().set_active_profile(profiles[index], &error)) {
                shortcutPanel_.status = "Profile: " + profiles[index];
                synchronize_menu_shortcuts();
            } else shortcutPanel_.status = error;
        }
        return;
    }
    if (normalized == "tab") {
        constexpr int count = static_cast<int>(ShortcutContext::ModalDialog) + 1;
        int value = shortcutPanel_.contextFilter ? static_cast<int>(*shortcutPanel_.contextFilter) : -1;
        value = shift ? value - 1 : value + 1;
        if (value < -1) value = count - 1;
        if (value >= count) value = -1;
        shortcutPanel_.contextFilter = value < 0 ? std::nullopt : std::optional<ShortcutContext>(static_cast<ShortcutContext>(value));
        shortcutPanel_.selectedRow = 0U;
        return;
    }
    if (normalized == "backspace") {
        if (!shortcutPanel_.searchQuery.empty()) shortcutPanel_.searchQuery.pop_back();
        shortcutPanel_.selectedRow = 0U;
        return;
    }
    if (normalized == "delete") {
        if (shortcutPanel_.selectedRow < rows.size()) {
            std::string error;
            (void)workspace_.shortcuts().set_binding(workspace_.shortcuts().active_profile(),
                rows[shortcutPanel_.selectedRow].command->actionId, rows[shortcutPanel_.selectedRow].context,
                shift ? ShortcutSlot::Secondary : ShortcutSlot::Primary, std::nullopt, false, &error);
            shortcutPanel_.status = error.empty() ? "Binding removed" : error;
            synchronize_menu_shortcuts();
        }
        return;
    }
    if (normalized == "f5") {
        std::string error;
        if (workspace_.shortcuts().reset_profile(workspace_.shortcuts().active_profile(), &error)) {
            shortcutPanel_.status = "Profile reset to defaults";
            synchronize_menu_shortcuts();
        } else shortcutPanel_.status = error;
        return;
    }
    if (normalized == "enter" || normalized == "return") {
        if (shortcutPanel_.selectedRow < rows.size()) {
            const ShortcutSearchResult& row = rows[shortcutPanel_.selectedRow];
            shortcutPanel_.capturing = true;
            shortcutPanel_.captureActionId = row.command->actionId;
            shortcutPanel_.captureContext = row.context;
            shortcutPanel_.captureSlot = shift ? ShortcutSlot::Secondary : ShortcutSlot::Primary;
            shortcutPanel_.status = "Press the new shortcut (Esc cancels)";
        }
    }
}

bool NativeEditorController::dispatch_shortcut_gesture(const ShortcutGesture& gesture) {
    const auto contexts = active_shortcut_contexts();
    const auto resolution = workspace_.shortcuts().resolve(gesture, contexts, settings_capabilities());
    if (!resolution) return false;
    return dispatch_action(resolution->actionId);
}


void NativeEditorController::resize(int width, int height) {
    width_ = std::max(640, width);
    height_ = std::max(480, height);
    recompute_layout();
}

void NativeEditorController::recompute_layout() {
    const float scale = kLogicalLayoutScale;
    const int menuHeight = static_cast<int>(28.0F * scale);
    const int toolbarHeight = static_cast<int>(40.0F * scale);
    const int statusHeight = static_cast<int>(22.0F * scale);
    const int bottomHeight = std::clamp(static_cast<int>(150.0F * scale), 90, std::max(90, height_ / 3));
    const int hierarchyWidth = workspace_.panel_visible(PanelId::SceneHierarchy)
        ? std::clamp(static_cast<int>(230.0F * scale), 160, std::max(160, width_ / 3))
        : 0;
    const int inspectorWidth = workspace_.panel_visible(PanelId::Inspector)
        ? std::clamp(static_cast<int>(270.0F * scale), 180, std::max(180, width_ / 3))
        : 0;
    layout_.menuBar = {0, 0, width_, menuHeight};
    layout_.toolbar = {0, menuHeight, width_, toolbarHeight};
    const int contentY = menuHeight + toolbarHeight;
    const int contentHeight = std::max(1, height_ - contentY - bottomHeight - statusHeight);
    layout_.hierarchy = {0, contentY, hierarchyWidth, contentHeight};
    layout_.inspector = {width_ - inspectorWidth, contentY, inspectorWidth, contentHeight};
    layout_.viewport = {hierarchyWidth, contentY, std::max(1, width_ - hierarchyWidth - inspectorWidth), contentHeight};
    layout_.bottomPanel = {0, contentY + contentHeight, width_, bottomHeight};
    layout_.statusBar = {0, height_ - statusHeight, width_, statusHeight};
    synthPanel_.resize(width_, height_, scale);
    chiptunePanel_.resize(width_, height_, scale);
    audioPanel_.resize(width_, height_, scale);
    audioEventPanel_.resize(width_, height_, scale);
    controlRigPanel_.resize(width_, height_);
    spriteAuthoringPanel_.resize(width_, height_);

    layout_.toolbarButtons.clear();
    const int buttonSize = std::max(28, toolbarHeight - 8);
    {
        // Share the toolbar between the nine tools, leaving room for the mode label on the
        // right. Buttons were a fixed 50 px, so "Select", "Remove" and "Anchor" ran past them.
        constexpr int kGap = 4;
        constexpr int kLeft = 8;
        const int modeReserve = static_cast<int>(96.0F * scale);
        const int tools = static_cast<int>(kEditorToolCount);
        const int available = width_ - kLeft - modeReserve - kGap * (tools - 1);
        const int buttonWidth = std::clamp(available / tools, 30, 84);
        int x = kLeft;
        for (int index = 0; index < tools; ++index) {
            layout_.toolbarButtons.push_back({x, menuHeight + 4, buttonWidth, buttonSize});
            x += buttonWidth + kGap;
        }
    }
    {
        const int helpWidth = static_cast<int>(92.0F * scale);
        const int helpHeight = std::max(18, static_cast<int>(20.0F * scale));
        layout_.viewportHelpButton = {layout_.viewport.x + layout_.viewport.width - helpWidth - 8,
                                      layout_.viewport.y + layout_.viewport.height - helpHeight - 8,
                                      helpWidth, helpHeight};
        if (layout_.viewport.width < helpWidth + 16 || layout_.viewport.height < helpHeight + 16)
            layout_.viewportHelpButton = {};
    }

    const int rowHeight = std::max(20, static_cast<int>(24.0F * scale));
    layout_.hierarchyFilterBox = {};
    layout_.hierarchyRows.clear();
    layout_.hierarchyDisclosureButtons.clear();
    if (hierarchyWidth > 0) {
        layout_.hierarchyFilterBox = {4, contentY + 4, hierarchyWidth - 8, rowHeight};
        const int firstRowY = contentY + rowHeight + static_cast<int>(10.0F * scale);
        const auto hierarchyOrder = hierarchy_order();
        const std::size_t rowCount = hierarchyOrder.size();
        std::set<EditorObjectId> parents;
        for (const auto& [id, object] : workspace_.document().objects()) {
            (void)id;
            if (object.parent) parents.insert(*object.parent);
        }
        // Rows used to continue past the panel into the bottom dock once a scene had more
        // objects than fit (easy to hit after Scatter). Only the rows that fit get a rect; the
        // wheel scrolls the window. Rows outside it are empty rects, so they are not clickable.
        const int visibleRows = std::max(1, (layout_.hierarchy.y + layout_.hierarchy.height - 4 - firstRowY) / rowHeight);
        layout_.hierarchyScrollMax = std::max(0, static_cast<int>(rowCount) - visibleRows);
        hierarchyScroll_ = std::clamp(hierarchyScroll_, 0, layout_.hierarchyScrollMax);
        layout_.hierarchyScroll = hierarchyScroll_;
        layout_.hierarchyVisibleRows = visibleRows;
        for (std::size_t i = 0; i < rowCount; ++i) {
            const int visibleIndex = static_cast<int>(i) - hierarchyScroll_;
            layout_.hierarchyDisclosureButtons.push_back({});
            if (visibleIndex < 0 || visibleIndex >= visibleRows) { layout_.hierarchyRows.push_back({}); continue; }
            const UiRect row{4, firstRowY + visibleIndex * rowHeight, hierarchyWidth - 8, rowHeight};
            layout_.hierarchyRows.push_back(row);
            if (parents.contains(hierarchyOrder[i])) {
                int depth = 0;
                const EditorObject* object = workspace_.document().find_object(hierarchyOrder[i]);
                for (auto parent = object ? object->parent : std::nullopt; parent && depth < 8;) {
                    ++depth;
                    const EditorObject* ancestor = workspace_.document().find_object(*parent);
                    parent = ancestor ? ancestor->parent : std::nullopt;
                }
                layout_.hierarchyDisclosureButtons.back() = {row.x + 8 + depth * 12, row.y, 14, row.height};
            }
        }
    }

    layout_.inspectorToggles.clear();
    layout_.inspectorFields.clear();
    layout_.scatterSettingRows.clear();
    layout_.inspectorStackedFields = inspectorWidth > 0 && inspectorWidth < 320;
    layout_.inspectorFieldCount = 0;
    if (inspectorWidth > 0 && duplicateSpecial_.active) {
        // Duplicate Special replaces the object details (see duplicate_panel_layout).
        layout_.inspectorContentClipY = layout_.inspector.y + layout_.inspector.height - 6;
        layout_.inspectorScroll = layout_.inspectorScrollMax = 0;
    } else if (inspectorWidth > 0 && scatter_settings_panel_active()) {
        // The scatter settings replace the object details (and the flag toggles) while the fill
        // preview is open or the Scatter brush is the tool: one row per setting.
        const int rowStep = std::max(20, static_cast<int>(22.0F * scale));
        const int firstRow = contentY + static_cast<int>(52.0F * scale);  // below the panel title
        const int inspectorBottom = layout_.inspector.y + layout_.inspector.height - 6;
        for (std::size_t index = 0; index < kScatterSettingFieldCount; ++index) {
            const UiRect row{layout_.inspector.x + 8, firstRow + static_cast<int>(index) * rowStep, inspectorWidth - 16, rowStep - 2};
            layout_.scatterSettingRows.push_back(row.y + row.height <= inspectorBottom ? row : UiRect{});
        }
        layout_.inspectorContentClipY = inspectorBottom;
        layout_.inspectorScroll = layout_.inspectorScrollMax = 0;
    } else if (inspectorWidth > 0) {
        // Matches the fixed y-offsets the renderer already draws the Position and Rotation
        // lines at (see apps/dve_native_editor_x11.cpp); kept here rather than computed from
        // font metrics because nothing else in this layout is font-metric-driven either.
        layout_.inspectorFields.push_back({layout_.inspector.x + 12, contentY + 80, inspectorWidth - 24, 18});
        layout_.inspectorFields.push_back({layout_.inspector.x + 12, contentY + 102, inspectorWidth - 24, 18});
        const auto selected = workspace_.selected_object();
        const EditorObject* selectedObject = selected ? workspace_.document().find_object(*selected) : nullptr;
        if (selectedObject && selectedObject->text3d) {
            for (int field = 0; field < 9; ++field) {
                layout_.inspectorFields.push_back({layout_.inspector.x + 12, contentY + 124 + field * 22,
                                                   inspectorWidth - 24, 18});
            }
        } else if (selectedObject && selectedObject->gaborVolume) {
            for (int field = 0; field < 6; ++field) {
                layout_.inspectorFields.push_back({layout_.inspector.x + 12, contentY + 124 + field * 22,
                                                   inspectorWidth - 24, 18});
            }
        } else {
            // Voxel size (plain voxel objects; see number_field_rects). The row is kept for any
            // other primary too, so a selection change never needs a relayout to show it.
            layout_.inspectorFields.push_back({layout_.inspector.x + 12, contentY + 124, inspectorWidth - 24, 18});
        }
        layout_.inspectorFieldCount = static_cast<int>(layout_.inspectorFields.size());
        const int reflowExtra = layout_.inspectorStackedFields ? 22 * layout_.inspectorFieldCount : 0;
        if (layout_.inspectorStackedFields) {
            for (std::size_t index = 0; index < layout_.inspectorFields.size(); ++index) {
                layout_.inspectorFields[index].y += static_cast<int>(index) * 22;
                layout_.inspectorFields[index].height = 40;
            }
        }
        // Last fixed detail line: baseline 377 (voxel/text3d) or 311 (Gabor) + descent/gap.
        const int contentBottom = contentY + static_cast<int>(
            (selectedObject && selectedObject->gaborVolume ? 326.0F : 390.0F) * scale) + reflowExtra;
        const int inspectorBottom = layout_.inspector.y + layout_.inspector.height - 6;
        // The toggles used to start at a fixed offset and simply continue downward, so at
        // 1280x719 (inspector ends at y=547) they spilled ~50 px into the bottom dock and
        // stole its clicks. Pick the first arrangement that fits below the details; if
        // none does, pin the block to the inspector bottom and clip the detail text above.
        struct ToggleArrangement { int columns; int rowHeight; int gap; };
        const std::array<ToggleArrangement, 4> arrangements{{
            {1, rowHeight, 4}, {1, 20, 2}, {2, rowHeight, 4}, {2, 20, 2}}};
        constexpr int kToggleCount = 5;
        ToggleArrangement chosen = arrangements.back();
        bool fits = false;
        const auto block_height = [&](const ToggleArrangement& a) {
            const int rows = (kToggleCount + a.columns - 1) / a.columns;
            return rows * a.rowHeight + (rows - 1) * a.gap;
        };
        for (const ToggleArrangement& a : arrangements) {
            if (contentBottom + block_height(a) <= inspectorBottom) { chosen = a; fits = true; break; }
        }
        const int blockHeight = block_height(chosen);
        // Never cover the name / ID / Position / Rotation lines (they end near +124).
        const int blockTop = fits ? contentBottom : std::max(contentY + kInspectorHeaderHeight + 40, inspectorBottom - blockHeight);
        const int columnGap = 8;
        const int columnWidth = (inspectorWidth - 24 - (chosen.columns - 1) * columnGap) / chosen.columns;
        for (int index = 0; index < kToggleCount; ++index) {
            const int row = index / chosen.columns;
            const int column = index % chosen.columns;
            const UiRect toggle{layout_.inspector.x + 12 + column * (columnWidth + columnGap),
                                blockTop + row * (chosen.rowHeight + chosen.gap), columnWidth, chosen.rowHeight};
            if (toggle.y + toggle.height > inspectorBottom) break;  // never spill into the dock
            layout_.inspectorToggles.push_back(toggle);
        }
        layout_.inspectorContentClipY = blockTop - 2;  // == contentBottom - 2 when it fits
        // Details that do not fit above the toggles scroll instead of being cut off.
        layout_.inspectorScrollMax = fits ? 0 : std::max(0, contentBottom - layout_.inspectorContentClipY);
        inspectorScroll_ = std::clamp(inspectorScroll_, 0, layout_.inspectorScrollMax);
        layout_.inspectorScroll = inspectorScroll_;
        const int detailTop = layout_.inspector.y + kInspectorHeaderHeight;
        for (std::size_t field = 0; field < layout_.inspectorFields.size(); ++field) {
            UiRect& rect = layout_.inspectorFields[field];
            rect.y -= inspectorScroll_;
            // Scrolled out of view -> not clickable.
            const int top = std::max(rect.y, detailTop);
            const int bottom = std::min(rect.y + rect.height, layout_.inspectorContentClipY);
            if (bottom <= top) rect = {};
            else { rect.y = top; rect.height = bottom - top; }
        }
    } else {
        layout_.inspectorContentClipY = layout_.inspector.y + layout_.inspector.height;
        layout_.inspectorScroll = layout_.inspectorScrollMax = 0;
    }

    layout_.bottomTabs.clear();
    static constexpr int kBottomTabCount = 6; // Problems, Tasks, Console, Profiler, Assets, Assistant
    const int tabWidth = std::max(70, static_cast<int>(90.0F * scale));
    for (int index = 0; index < kBottomTabCount; ++index) {
        layout_.bottomTabs.push_back({layout_.bottomPanel.x + 8 + index * tabWidth, layout_.bottomPanel.y + 4, tabWidth - 6, rowHeight});
    }
    layout_.assetSearchBox = {};
    layout_.assetRefreshButton = {};
    layout_.assetFilterButton = {};
    layout_.assetRows.clear();
    layout_.aiPromptBox = {};
    layout_.aiSendButton = {};
    layout_.aiApproveButton = {};
    layout_.aiDenyButton = {};
    if (bottomTab_ == BottomPanelTab::Assistant) {
        const int promptY = layout_.bottomPanel.y + layout_.bottomPanel.height - rowHeight - 8;
        layout_.aiSendButton = {layout_.bottomPanel.x + layout_.bottomPanel.width - 84, promptY, 74, rowHeight};
        layout_.aiPromptBox = {layout_.bottomPanel.x + 10, promptY, std::max(40, layout_.bottomPanel.width - 104), rowHeight};
        if (!workspace_.ai_assistant().pending_approvals().empty()) {
            const int approvalY = promptY - rowHeight - 5;
            layout_.aiDenyButton = {layout_.bottomPanel.x + layout_.bottomPanel.width - 84, approvalY, 74, rowHeight};
            layout_.aiApproveButton = {layout_.aiDenyButton.x - 82, approvalY, 74, rowHeight};
        }
    }
    if (bottomTab_ == BottomPanelTab::Assets) {
        const int controlsY = layout_.bottomPanel.y + rowHeight + 10;
        layout_.assetRefreshButton = {layout_.bottomPanel.x + layout_.bottomPanel.width - 84, controlsY, 74, rowHeight};
        layout_.assetFilterButton = {layout_.assetRefreshButton.x - 96, controlsY, 88, rowHeight};
        layout_.assetSearchBox = {layout_.bottomPanel.x + 10, controlsY,
                                  std::max(80, layout_.assetFilterButton.x - layout_.bottomPanel.x - 20), rowHeight};
        const auto rows = asset_browser_rows();
        int assetY = controlsY + rowHeight + 22;
        for (std::size_t i = assetBrowserState_.firstVisible; i < rows.size(); ++i) {
            layout_.assetRows.push_back({layout_.bottomPanel.x + 8, assetY, layout_.bottomPanel.width - 16, rowHeight});
            assetY += rowHeight;
            if (assetY > layout_.bottomPanel.y + layout_.bottomPanel.height - rowHeight) break;
        }
    }
}

struct NativeEditorController::AutosaveJob {
    std::uint64_t manualSaveCount{};  // File > Save count when this autosave started
    std::mutex mutex;
    bool done{};
    bool success{};
    std::string error;
    std::filesystem::path manifest;
};

namespace {
std::string autosave_stem(const EditorDocument& document) {
    std::string stem = !document.path().empty() ? document.path().stem().string() : document.name();
    for (char& c : stem)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
    return stem.empty() ? std::string("untitled") : stem;
}
} // namespace

std::filesystem::path NativeEditorController::autosave_directory() const {
    std::filesystem::path base = projectRoot_;
    if (base.empty() && !workspace_.document().path().empty()) base = workspace_.document().path().parent_path();
    if (base.empty()) {
        std::error_code error;
        base = std::filesystem::current_path(error);
    }
    return base / ".dve" / "recovery" / autosave_stem(workspace_.document());
}

std::filesystem::path NativeEditorController::autosave_manifest_path() const {
    const auto& path = workspace_.document().path();
    const std::string fileName = !path.empty() && path.has_filename()
        ? path.filename().string() : autosave_stem(workspace_.document()) + ".dvescene";
    return autosave_directory() / fileName;
}

void NativeEditorController::report_recovered_autosave() {
    std::error_code error;
    const auto recovery = autosave_manifest_path();
    if (!std::filesystem::exists(recovery, error)) return;
    const auto& scene = workspace_.document().path();
    if (!scene.empty() && std::filesystem::exists(scene, error)) {
        const auto sceneTime = std::filesystem::last_write_time(scene, error);
        if (!error && sceneTime >= std::filesystem::last_write_time(recovery, error) && !error) return;
    }
    const std::string message = "Unsaved changes from an earlier session were autosaved to " +
        recovery.string() + "; open that scene with --scene to recover them.";
    workspace_.log().add(EditorLogLevel::Warning, message);
    set_status("Autosaved changes from an earlier session are available (see Console)", false, 10.0F);
}

void NativeEditorController::autosave_tick(float elapsedSeconds) {
    if (autosaveInFlight_) {
        std::lock_guard lock(autosaveInFlight_->mutex);
        if (autosaveInFlight_->done) {
            autosaveStatus_.inFlight = false;
            if (autosaveInFlight_->success && autosaveInFlight_->manualSaveCount != manualSaveCount_) {
                // The scene was saved while this ran; its copy is older than the file.
                std::error_code ignored;
                std::filesystem::remove_all(autosaveInFlight_->manifest.parent_path(), ignored);
            } else if (autosaveInFlight_->success) {
                ++autosaveStatus_.completed;
                autosaveStatus_.lastManifest = autosaveInFlight_->manifest;
                autosaveStatus_.lastError.clear();
                workspace_.log().add(EditorLogLevel::Info, "Autosaved to " + autosaveInFlight_->manifest.string());
            } else {
                ++autosaveStatus_.failed;
                autosaveStatus_.lastError = autosaveInFlight_->error;
                workspace_.log().add(EditorLogLevel::Warning, "Autosave failed: " + autosaveInFlight_->error);
            }
        }
    }
    if (autosaveInFlight_ && !autosaveStatus_.inFlight) autosaveInFlight_.reset();
    if (!recoveryChecked_) {
        recoveryChecked_ = true;
        report_recovered_autosave();
    }
    if (!workspace_.document().dirty()) {
        autosaveElapsedSeconds_ = 0.0;
        return;
    }
    autosaveElapsedSeconds_ += std::max(0.0F, elapsedSeconds);
    const SettingValue minutesValue = workspace_.settings().value("editor.autosave_minutes");
    const std::int64_t minutes = std::clamp<std::int64_t>(
        std::get_if<std::int64_t>(&minutesValue) ? std::get<std::int64_t>(minutesValue) : 5, 1, 120);
    if (polygonPreviewBefore_ || polygonDragBefore_ || mesh_authoring_dragging()) return;
    if (autosaveInFlight_ || autosaveElapsedSeconds_ < static_cast<double>(minutes) * 60.0) return;
    autosaveElapsedSeconds_ = 0.0;

    // Clone on this thread (a consistent snapshot; whole-brick copies keep it quick),
    // then write it on the worker. The open document, its path, revision and dirty
    // flag are untouched. The copy is staged and swapped in only once fully written,
    // so a failed or interrupted autosave never destroys the previous one.
    auto job = std::make_shared<AutosaveJob>();
    job->manualSaveCount = manualSaveCount_;
    auto snapshot = std::make_shared<EditorDocument>(clone_editor_document(workspace_.document()));
    const auto manifest = autosave_manifest_path();
    std::string submitError;
    const auto task = autosaveTasks_.submit("Autosave scene", [job, snapshot, manifest,wake=wakeCallback_](EditorTaskContext&) {
        bool success = false;
        std::string error;
        try {
            const auto folder = manifest.parent_path();
            const std::filesystem::path staging = folder.string() + ".writing";
            std::filesystem::remove_all(staging);
            std::filesystem::create_directories(staging);
            const auto result = snapshot->save_transactional(staging / manifest.filename());
            if (result.success) {
                std::filesystem::remove_all(folder);
                std::filesystem::rename(staging, folder);
                success = true;
            } else {
                error = result.error;
                std::error_code cleanup;
                std::filesystem::remove_all(staging, cleanup);
            }
        } catch (const std::exception& exception) {
            error = exception.what();
        }
        {
            std::lock_guard lock(job->mutex);
            job->done = true; job->success = success;
            job->error = std::move(error); job->manifest = manifest;
        }
        if(wake) wake();
    }, &submitError);
    if (!task) {
        ++autosaveStatus_.failed;
        autosaveStatus_.lastError = submitError;
        workspace_.log().add(EditorLogLevel::Warning, "Autosave could not start: " + submitError);
        return;
    }
    autosaveInFlight_ = std::move(job);
    autosaveStatus_.inFlight = true;
}

void NativeEditorController::update(float elapsedSeconds) {
    if (appliedSettingsRevision_ != workspace_.settings().revision()) apply_settings_to_runtime();
    else if (audioSettingsPending_) {
        audioSettingsPending_ = !apply_audio_settings_to_runtime();
        if (!audioSettingsPending_) {
            const auto position = status_.text.find("; audio changes pending");
            if (position != std::string::npos) status_.text.erase(position, 23);
        }
    }
    if (navigationShortcutProfile_ != workspace_.shortcuts().active_profile()) {
        clear_navigation_input();
        navigationShortcutProfile_ = workspace_.shortcuts().active_profile();
    }
    autosave_tick(elapsedSeconds);
    poll_long_job();
    voxel_boolean_tick();
    prune_voxel_selection();
    prune_polygon_selection();
    if (duplicateSpecial_.active) {
        bool gone = playSession_.active() || selectionDomain_ != SelectionDomain::Objects;
        for (const EditorObjectId id : duplicateSpecial_.roots) gone = gone || !workspace_.document().find_object(id);
        if (gone) cancel_duplicate_special("Duplicate Special closed: the objects or mode changed");
    }
    prune_groups();
    if (followSelection_) {
        // Keep the camera's offset to the selection: move eye and target by how far it moved.
        if (workspace_.selected_objects().empty() || playSession_.active()) {
            followSelection_ = false;
            followAnchor_.reset();
            set_status(playSession_.active() ? "Stopped following the selection: Play started"
                                             : "Stopped following: nothing selected");
            refresh_menu_state();
        } else if (!navigation_pointer_active()) {
            const Float3 pivot = selection_pivot();
            if (followAnchor_) {
                const Float3 delta = subtract(pivot, *followAnchor_);
                if (length_squared(delta) > 1.0e-10F) {
                    camera_.position = add(camera_.position, delta);
                    camera_.target = add(camera_.target, delta);
                }
            }
            followAnchor_ = pivot;
        }
    }
    synthPanel_.flush_wavetable_draft_if_due(audioMixer_.synthesizer());
    synthPanel_.sync_wavetable_section(audioMixer_.synthesizer());
    refresh_midi_status();
    if (midiOutput_ && midiOutput_->connected()) {
        audio::MidiMessage message;
        while (audioMixer_.synthesizer().poll_midi_output(message)) (void)midiOutput_->send(message);
    }
    spriteAuthoringPanel_.update(elapsedSeconds);
    controlRigPanel_.update(elapsedSeconds);
    if (tileWorldEditorOpen_) ++tileWorldEditorTicks_;
    if (spriteRig2DOpen_ && spriteRig2DPreviewPlaying_) {
        spriteRig2DPreviewTime_ += std::clamp(elapsedSeconds, 0.0F, 0.25F);
        const auto& clips = spriteRig2D_.asset().clips;
        if (!clips.empty() && clips.front().durationSeconds > 0.0F)
            spriteRig2DPreviewTime_ = std::fmod(spriteRig2DPreviewTime_, clips.front().durationSeconds);
    }
    if (spriteLevelPlaying_ && spriteLevel_ && spriteLevelPresentation_) {
        constexpr float fixedStep = 1.0F / 60.0F;
        const auto steps = spriteLevelClock_.advance(elapsedSeconds);
        for (std::uint32_t step = 0; step < steps; ++step) {
            spriteLevel_->step(spriteLevelInput_, fixedStep);
            spriteLevelPresentation_->sync(*spriteLevel_, fixedStep);
            spriteLevelInput_.jumpPressed = false;
            spriteLevelInput_.firePressed = false;
            spriteLevelInput_.savePressed = false;
            spriteLevelInput_.loadPressed = false;
            spriteLevelInput_.restartPressed = false;
        }

    }
    if (spriteAnimationGraphOpen_ && spriteLevel_) {
        const auto& state = spriteLevel_->state();
        spriteAnimationGraph_.set_live_state(!state.grounded ? "Jump" :
            (std::fabs(state.velocity.x) > 2.0F ? "Run" : "Idle"));
    }
    asset_scan_tick();
    if (thumbnailBacklog_ && !assetScanInFlight_) {
        std::string thumbnailError;
        thumbnailBacklog_ = !assetDatabase_.generate_missing_thumbnails_for(
            std::chrono::milliseconds(2), nullptr, &thumbnailError);
        if (!thumbnailError.empty()) {
            // Log once and stop; an unwritable thumbnail folder would otherwise fail every frame.
            workspace_.log().add(EditorLogLevel::Warning, "Asset thumbnails paused: " + thumbnailError);
            thumbnailBacklog_ = false;
        }
    }
    if (liveMcpHost_) (void)liveMcpHost_->pump(16);
    workspace_.ai_assistant().refresh_pending_approvals();
    if (workspace_.ai_assistant().poll()) {
        const bool failed = !workspace_.ai_assistant().last_error().empty();
        set_status(failed ? workspace_.ai_assistant().last_error() : "AI Assistant response received",
                   failed, failed ? 6.0F : 3.0F);
    }
    if (playSession_.active()) {
        std::string playError;
        if (!playSession_.update(workspace_, elapsedSeconds, &playError)) {
            set_status(playError.empty() ? "Play-in-editor update failed" : playError, true, 6.0F);
            (void)stop_play_session(false);
        }
    }
    // Camera rigs follow or look at scene objects by id. Only the targets some rig names are
    // read, so only those are refreshed: refreshing every object each frame cost about
    // 1.4 ms at 10,000 objects. A named object that no longer exists is removed as a target,
    // so a rig stops tracking a deleted object instead of its last position.
    ++cameraTargetGeneration_;
    for (const camera::CameraRig& rig : cameraDirector_.rigs()) {
        for (const std::optional<camera::CameraTargetId>& targetId : {rig.followTarget, rig.lookAtTarget}) {
            if (!targetId) continue;
            const EditorObject* object = workspace_.document().find_object(*targetId);
            if (object == nullptr) {
                cameraDirector_.remove_target(*targetId);
                continue;
            }
            camera::CameraTargetState target;
            target.position = object->transform.position;
            target.forward = rotate(object->transform.rotation, {0.0F, 0.0F, -1.0F});
            target.up = rotate(object->transform.rotation, {0.0F, 1.0F, 0.0F});
            auto& history = cameraTargetHistory_[*targetId];
            if (history.generation != cameraTargetGeneration_) {
                history.velocity = history.generation != 0U && history.generation + 1U == cameraTargetGeneration_ && elapsedSeconds > 0.0F
                    ? multiply(subtract(target.position, history.position), 1.0F / elapsedSeconds) : Float3{};
                history.position = target.position;
                history.generation = cameraTargetGeneration_;
            }
            target.velocity = history.velocity;
            cameraDirector_.set_target(*targetId, target);
        }
    }
    for (auto it = cameraTargetHistory_.begin(); it != cameraTargetHistory_.end();) {
        if (it->second.generation != cameraTargetGeneration_) it = cameraTargetHistory_.erase(it);
        else ++it;
    }
    if (navigationCameraRig_ != selectedCameraRig_) reset_camera_navigation();
    if (!sliceDrag_ && elapsedSeconds > 0.0F && (!heldShortcutGestures_.empty() || cameraNavigationFilter_.has_motion() ||
                                 pendingCameraLook_[0] != 0.0F || pendingCameraLook_[1] != 0.0F)) {
        const auto contexts = active_shortcut_contexts();
        const bool flyContext = std::find(contexts.begin(), contexts.end(), ShortcutContext::FlyNavigation) != contexts.end();
        const bool cameraContext = flyContext || std::find(contexts.begin(), contexts.end(), ShortcutContext::Camera) != contexts.end();
        if (!cameraContext) reset_camera_navigation();
        if (cameraContext && (activePointerCommand_ == "viewport.look" || activePointerCommand_ == "viewport.orbit")) {
            const float fraction = cameraInputSmoothing_ > 0.0F
                ? -std::expm1(-elapsedSeconds / cameraInputSmoothing_) : 1.0F;
            const float dx = pendingCameraLook_[0] * fraction, dy = pendingCameraLook_[1] * fraction;
            pendingCameraLook_[0] -= dx;
            pendingCameraLook_[1] -= dy;
            for (auto& residual : pendingCameraLook_)
                if (std::fabs(residual) < 1.0e-7F) residual = 0.0F;
            if (activePointerCommand_ == "viewport.look") look_camera(camera_, dx, dy, 1.0F);
            else orbit_camera(camera_, dx, dy, 1.0F);
        } else pendingCameraLook_ = {};
        if (flyContext) {
            Float3 movement{};
            bool boosted = false;
            for (const ShortcutGesture& gesture : heldShortcutGestures_) {
                const auto resolution = workspace_.shortcuts().resolve(gesture, contexts, settings_capabilities());
                if (!resolution) continue;
                if (resolution->actionId == "camera.fly_forward") movement.z += 1.0F;
                else if (resolution->actionId == "camera.fly_backward") movement.z -= 1.0F;
                else if (resolution->actionId == "camera.fly_left") movement.x -= 1.0F;
                else if (resolution->actionId == "camera.fly_right") movement.x += 1.0F;
                else if (resolution->actionId == "camera.fly_down") movement.y -= 1.0F;
                else if (resolution->actionId == "camera.fly_up") movement.y += 1.0F;
                else continue;
                boosted = boosted || gesture.shift;
            }
            {
                const SettingValue speedValue = workspace_.settings().value("camera.fly_speed");
                const SettingValue boostValue = workspace_.settings().value("camera.boost_multiplier");
                const float speed = std::get_if<double>(&speedValue)
                    ? static_cast<float>(std::get<double>(speedValue)) : workspace_.preferences().cameraSpeed;
                const float boost = std::get_if<double>(&boostValue)
                    ? static_cast<float>(std::get<double>(boostValue)) : 4.0F;
                const Float3 velocity = length_squared(movement) > 0.0F
                    ? multiply(normalize(movement), speed * (boosted ? boost : 1.0F)) : Float3{};
                const Float3 displacement = cameraNavigationFilter_.integrate(velocity, elapsedSeconds,
                    cameraInputAcceleration_, cameraInputSmoothing_);
                fly_camera(camera_, displacement, 1.0F, 1.0F);
            }
        } else cameraNavigationFilter_.reset();
    }
    float cameraElapsed = std::max(0.0F, elapsedSeconds);
    const auto ignoreScale = workspace_.settings().value("camera.ignore_time_scale");
    if (playSession_.active() && !std::get<bool>(ignoreScale))
        cameraElapsed = playSession_.paused() ? 0.0F : cameraElapsed * playSession_.config().timeScale;
    (void)cameraDirector_.update(cameraElapsed, playSession_.camera_collision_world());
    const SettingValue lockCameraValue = workspace_.settings().value("camera.lock_viewport_to_selected");
    const bool lockCamera = std::get_if<bool>(&lockCameraValue) && std::get<bool>(lockCameraValue);
    if (lockCamera && selectedCameraRig_) {
        if (cameraDirector_.find_rig(*selectedCameraRig_))
            apply_camera_pose(camera_, cameraDirector_.current_pose());
    }
    if (std::get<bool>(workspace_.settings().value("camera.horizon_lock")))
        camera_.worldUp = {0.0F, 1.0F, 0.0F};
    if (status_.secondsRemaining > 0.0F) {
        status_.secondsRemaining = std::max(0.0F, status_.secondsRemaining - std::max(0.0F, elapsedSeconds));
        if (status_.secondsRemaining == 0.0F && status_.text != "Ready") status_ = {"Ready", false, 0.0F};
    }
}

bool NativeEditorController::animating() const noexcept {
    return playSession_.state()==EditorPlaySessionState::Running || spriteLevelPlaying_ || (spriteRig2DOpen_ && spriteRig2DPreviewPlaying_) ||
           spriteAuthoringPanel_.open() || cameraDirector_.telemetry().blendWeight < 1.0F ||
           !heldShortcutGestures_.empty() || audioMixer_.synthesizer().meters().activeVoices > 0U ||
           thumbnailBacklog_;
}

double NativeEditorController::idle_update_seconds() const noexcept {
    // Hotplug discovery is polling-based. Everything with a completion callback wakes
    // immediately; this deadline keeps device discovery and autosave time progressing.
    double seconds=1.0;
    if(status_.secondsRemaining>0.0F) seconds=std::min(seconds,double(status_.secondsRemaining));
    return std::max(0.001,seconds);
}

void NativeEditorController::set_status(std::string text, bool error, float seconds) {
    status_ = {std::move(text), error, seconds};
    workspace_.log().add(error ? EditorLogLevel::Error : EditorLogLevel::Info, status_.text);
}

std::string_view NativeEditorController::pending_confirmation_message() const noexcept {
    switch (pendingDestructiveAction_) {
        case PendingDestructiveAction::Quit:
            return "Unsaved changes will be lost. Enter to quit, Escape to cancel.";
        case PendingDestructiveAction::NewProject:
            return "Unsaved changes will be lost. Enter for a new project, Escape to cancel.";
        case PendingDestructiveAction::NewScene:
            return "Unsaved changes will be lost. Enter for a new scene, Escape to cancel.";
        case PendingDestructiveAction::OpenScene:
            return "Unsaved changes will be lost. Enter to open the other scene, Escape to cancel.";
        case PendingDestructiveAction::OpenProject:
            return "Unsaved changes will be lost. Enter to open the other project, Escape to cancel.";
        case PendingDestructiveAction::Inactive:
            break;
    }
    return {};
}

void NativeEditorController::request_quit() {
    if (quitRequested_) return;
    if (workspace_.document().dirty() && workspace_.preferences().confirmDestructiveActions) {
        pendingDestructiveAction_ = PendingDestructiveAction::Quit;
        return;
    }
    save_workspace_state_on_quit();
    quitRequested_ = true;
}

void NativeEditorController::create_new_project_now() {
    reset_hierarchy_view();
    workspace_.document() = make_new_project_document();
    workspace_.commands().clear();
    workspace_.clear_selection();
    reset_voxel_selection_state();
    openGroups_.clear(); pivotEditing_ = false;  // ids restart per scene
    if (!workspace_.document().objects().empty()) {
        workspace_.select_object(workspace_.document().objects().begin()->first);
        frame_selection();
    }
    recompute_layout();
}

void NativeEditorController::create_new_scene_now() {
    reset_hierarchy_view();
    workspace_.document() = make_new_scene_document();
    workspace_.commands().clear();
    workspace_.clear_selection();
    reset_voxel_selection_state();
    openGroups_.clear(); pivotEditing_ = false;  // ids restart per scene
    if (!workspace_.document().objects().empty()) {
        workspace_.select_object(workspace_.document().objects().begin()->first);
        frame_selection();
    }
    recompute_layout();
}

void NativeEditorController::confirm_pending_destructive_action() {
    const PendingDestructiveAction action = pendingDestructiveAction_;
    pendingDestructiveAction_ = PendingDestructiveAction::Inactive;
    if (action == PendingDestructiveAction::Quit) {
        save_workspace_state_on_quit();
        quitRequested_ = true;
    } else if (action == PendingDestructiveAction::NewProject) {
        create_new_project_now();
        set_status("New project");
    } else if (action == PendingDestructiveAction::NewScene) {
        create_new_scene_now();
        set_status("New scene");
    } else if (action == PendingDestructiveAction::OpenScene || action == PendingDestructiveAction::OpenProject) {
        const std::filesystem::path path = std::exchange(pendingOpenPath_, {});
        if (action == PendingDestructiveAction::OpenScene) (void)open_scene_now(path);
        else (void)open_project_now(path);
    }
}

void NativeEditorController::cancel_pending_destructive_action() {
    if (pendingDestructiveAction_ == PendingDestructiveAction::Inactive) return;
    pendingDestructiveAction_ = PendingDestructiveAction::Inactive;
    set_status("Destructive action cancelled");
}

std::optional<std::string> NativeEditorController::menu_name_at(int x) const {
    const int desiredWidth = static_cast<int>(78.0F * kLogicalLayoutScale);
    const int itemWidth = std::max(1, std::min(desiredWidth, width_ / static_cast<int>(kMenuBarNames.size())));
    const int index = x / itemWidth;
    if (index < 0 || index >= static_cast<int>(kMenuBarNames.size())) return std::nullopt;
    return std::string(kMenuBarNames[static_cast<std::size_t>(index)]);
}

NativeMenuPopupLayout NativeEditorController::menu_popup_layout() const {
    NativeMenuPopupLayout result;
    if (!openMenu_) return result;
    const auto actions = menu_actions(*openMenu_);
    if (actions.empty()) return result;
    const int itemHeight = static_cast<int>(24.0F * kLogicalLayoutScale);
    int menuIndex = 0;
    for (std::size_t candidate = 0; candidate < kMenuBarNames.size(); ++candidate) {
        if (kMenuBarNames[candidate] == *openMenu_) {
            menuIndex = static_cast<int>(candidate);
            break;
        }
    }
    const int desiredWidth = static_cast<int>(78.0F * kLogicalLayoutScale);
    const int menuWidth = std::max(1, std::min(desiredWidth, width_ / static_cast<int>(kMenuBarNames.size())));
    const int popupWidth = std::clamp(static_cast<int>(300.0F * kLogicalLayoutScale), 240, 420);
    const int popupX = std::clamp(menuIndex * menuWidth, 0, std::max(0, width_ - popupWidth));
    const int availableHeight = std::max(itemHeight, height_ - layout_.menuBar.height - layout_.statusBar.height - 4);
    const std::size_t visibleCount = std::max<std::size_t>(
        1U, std::min(actions.size(), static_cast<std::size_t>(availableHeight / itemHeight)));
    const std::size_t maximumOffset = actions.size() > visibleCount ? actions.size() - visibleCount : 0U;
    result.firstVisibleAction = std::min(menuScrollOffset_, maximumOffset);
    result.canScrollUp = result.firstVisibleAction > 0U;
    result.canScrollDown = result.firstVisibleAction + visibleCount < actions.size();
    result.popup = {popupX, layout_.menuBar.height, popupWidth,
                    itemHeight * static_cast<int>(visibleCount)};
    result.rows.reserve(visibleCount);
    for (std::size_t visible = 0; visible < visibleCount; ++visible) {
        result.rows.push_back({result.popup.x, result.popup.y + static_cast<int>(visible) * itemHeight,
                               result.popup.width, itemHeight});
    }
    return result;
}

void NativeEditorController::open_top_level_menu(std::string menuName) {
    if (openMenu_ && *openMenu_ == menuName) return;
    openMenu_ = std::move(menuName);
    menuScrollOffset_ = 0U;
    menuHoveredAction_.reset();
    const auto actions = menu_actions(*openMenu_);
    const auto firstEnabled = std::find_if(actions.begin(), actions.end(), [](const MenuAction& action) {
        return action.enabled;
    });
    if (firstEnabled != actions.end()) {
        menuHoveredAction_ = static_cast<std::size_t>(std::distance(actions.begin(), firstEnabled));
    }
}

void NativeEditorController::close_top_level_menu() noexcept {
    openMenu_.reset();
    menuHoveredAction_.reset();
    menuScrollOffset_ = 0U;
}

void NativeEditorController::move_menu_selection(int direction) {
    if (!openMenu_ || direction == 0) return;
    const auto actions = menu_actions(*openMenu_);
    if (actions.empty()) return;
    std::size_t index = menuHoveredAction_.value_or(direction > 0 ? actions.size() - 1U : 0U);
    for (std::size_t attempt = 0; attempt < actions.size(); ++attempt) {
        index = direction > 0 ? (index + 1U) % actions.size()
                              : (index + actions.size() - 1U) % actions.size();
        if (actions[index].enabled) {
            menuHoveredAction_ = index;
            const NativeMenuPopupLayout popup = menu_popup_layout();
            const std::size_t visibleCount = popup.rows.size();
            if (index < menuScrollOffset_) menuScrollOffset_ = index;
            else if (visibleCount > 0U && index >= menuScrollOffset_ + visibleCount)
                menuScrollOffset_ = index - visibleCount + 1U;
            return;
        }
    }
}

void NativeEditorController::switch_top_level_menu(int direction) {
    if (!openMenu_ || direction == 0) return;
    std::size_t index = 0U;
    for (; index < kMenuBarNames.size(); ++index) if (kMenuBarNames[index] == *openMenu_) break;
    if (index == kMenuBarNames.size()) index = 0U;
    index = direction > 0 ? (index + 1U) % kMenuBarNames.size()
                          : (index + kMenuBarNames.size() - 1U) % kMenuBarNames.size();
    open_top_level_menu(std::string(kMenuBarNames[index]));
}

bool NativeEditorController::activate_menu_selection() {
    if (!openMenu_ || !menuHoveredAction_) return false;
    const auto actions = menu_actions(*openMenu_);
    if (*menuHoveredAction_ >= actions.size() || !actions[*menuHoveredAction_].enabled) return false;
    const std::string actionId = actions[*menuHoveredAction_].id;
    close_top_level_menu();
    return dispatch_action(actionId);
}

namespace {
std::string format_float3_line(Float3 value) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%.3f %.3f %.3f", static_cast<double>(value.x),
                  static_cast<double>(value.y), static_cast<double>(value.z));
    return buffer;
}
[[nodiscard]] std::optional<Float3> parse_float3_line(std::string_view text) {
    float values[3]{};
    int count = 0;
    std::size_t cursor = 0;
    const auto separator = [](unsigned char c) noexcept {
        return std::isspace(c) != 0 || c == ',';
    };
    while (cursor < text.size() && count < 3) {
        while (cursor < text.size() && separator(static_cast<unsigned char>(text[cursor]))) ++cursor;
        if (cursor >= text.size()) break;
        std::size_t end = cursor;
        while (end < text.size() && !separator(static_cast<unsigned char>(text[end]))) ++end;
        const std::string token(text.substr(cursor, end - cursor));
        char* parsedEnd = nullptr;
        const float parsed = std::strtof(token.c_str(), &parsedEnd);
        if (parsedEnd == token.c_str() || parsedEnd != token.c_str() + token.size() || !std::isfinite(parsed))
            return std::nullopt;
        values[count++] = parsed;
        cursor = end;
    }
    while (cursor < text.size() && separator(static_cast<unsigned char>(text[cursor]))) ++cursor;
    if (count != 3 || cursor != text.size()) return std::nullopt;
    return Float3{values[0], values[1], values[2]};
}
[[nodiscard]] std::optional<float> parse_float_line(std::string_view text) {
    const std::string token(text);
    char* end = nullptr;
    const float value = std::strtof(token.c_str(), &end);
    if (end == token.c_str() || end != token.c_str() + token.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

[[nodiscard]] std::optional<std::uint32_t> parse_uint_line(std::string_view text) {
    if (text.empty() || text.front() == '-') return std::nullopt;
    const std::string token(text);
    char* end = nullptr;
    const unsigned long long value = std::strtoull(token.c_str(), &end, 10);
    if (end == token.c_str() || end != token.c_str() + token.size() ||
        value > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    return static_cast<std::uint32_t>(value);
}

} // namespace

void NativeEditorController::begin_text_edit(TextEditKind kind, EditorObjectId objectId, std::string initialBuffer) {
    if (textEdit_.kind != TextEditKind::Inactive) commit_text_edit();
    textEdit_.kind = kind;
    textEdit_.objectId = objectId;
    textEdit_.buffer = std::move(initialBuffer);
    textEdit_.error.clear();
    textEdit_.replaceOnNextInput = true;
    textEdit_.transformSelection.clear();
    textEdit_.transformSnapshot.clear();
    if (kind == TextEditKind::Position || kind == TextEditKind::Rotation) {
        textEdit_.transformSelection = workspace_.selected_objects();
        textEdit_.transformSnapshot = selection_transform_snapshot();
        textEdit_.transformPivot = selection_pivot();
        set_status(kind == TextEditKind::Position
            ? "Position in meters: moves selection by the primary object's delta"
            : "Rotation in degrees: rotates selection together about the current pivot", false, 8.0F);
    }
    close_context_menu();
    commandPaletteOpen_ = false;
    close_top_level_menu();
}

void NativeEditorController::commit_text_edit() {
    const TextEditKind kind = textEdit_.kind;
    if (kind == TextEditKind::Inactive) return;
    if (kind == TextEditKind::SlicePoint || kind == TextEditKind::SliceNormal) {
        if (!voxelSlice_.active() || voxelSlice_.target() != textEdit_.objectId ||
            workspace_.selected_object() != textEdit_.objectId) { cancel_text_edit(); return; }
        const auto parsed = parse_float3_line(textEdit_.buffer);
        if (!parsed || !configure_voxel_slice(kind == TextEditKind::SlicePoint ? *parsed : voxelSlice_.point(),
                kind == TextEditKind::SliceNormal ? *parsed : voxelSlice_.normal(), voxelSlice_.output())) {
            textEdit_.error = "Enter three finite numbers; the normal must be nonzero.";
            set_status(textEdit_.error, true);
            return;
        }
        textEdit_ = {};
        return;
    }
    if (kind == TextEditKind::HierarchyFilter) {
        textEdit_ = {};
        return;
    }
    if (kind == TextEditKind::AssetSearch) {
        assetBrowserState_.query.text = textEdit_.buffer;
        assetBrowserState_.firstVisible = 0U;
        textEdit_ = {};
        recompute_layout();
        return;
    }
    if (kind == TextEditKind::AssetRename) {
        const std::string name = textEdit_.buffer;
        textEdit_ = {};
        if (!assetBrowserState_.selectedId) {
            set_status("No asset selected", true);
            return;
        }
        const EditorAssetRecord* selectedAsset = std::as_const(assetDatabase_).find(*assetBrowserState_.selectedId);
        if (!selectedAsset || name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
            set_status("Asset rename requires a valid filename", true);
            return;
        }
        const auto target = selectedAsset->relativePath.parent_path() / name;
        (void)finish_asset_scan();  // the move rewrites the index; never alongside a scan
        const auto moved = assetDatabase_.move_asset(*assetBrowserState_.selectedId, target, true);
        set_status(moved.success ? moved.message : moved.message, !moved.success, 7.0F);
        recompute_layout();
        return;
    }
    if (kind == TextEditKind::ScatterSetting) {
        if (!scatter_settings_panel_active()) { textEdit_ = {}; recompute_layout(); return; }  // panel went away (e.g. Play)
        const auto field = static_cast<ScatterSettingField>(std::min<EditorObjectId>(textEdit_.objectId, kScatterSettingFieldCount - 1U));
        std::string error;
        if (!set_scatter_setting(field, textEdit_.buffer, &error)) {
            textEdit_.error = error;
            set_status(error, true);
            return;
        }
        textEdit_ = {};
        return;
    }
    if (kind == TextEditKind::AiPrompt) {
        const std::string prompt = textEdit_.buffer;
        textEdit_ = {};
        (void)submit_ai_prompt(prompt);
        return;
    }
    if (kind == TextEditKind::ObjectName) {
        const EditorObject* object = workspace_.document().find_object(textEdit_.objectId);
        if (object && textEdit_.buffer != object->name && !textEdit_.buffer.empty()) {
            const CommandResult result = workspace_.commands().execute(
                workspace_.document(),
                std::make_unique<RenameObjectCommand>(textEdit_.objectId, object->name, textEdit_.buffer));
            set_status(result.success ? "Renamed object" : result.message, !result.success);
        }
        textEdit_ = {};
        return;
    }
    const bool text3dProperty = kind == TextEditKind::Text3DContent ||
        kind == TextEditKind::Text3DSize || kind == TextEditKind::Text3DDepth ||
        kind == TextEditKind::Text3DLetterSpacing || kind == TextEditKind::Text3DFaceMaterial ||
        kind == TextEditKind::Text3DSideMaterial || kind == TextEditKind::Text3DFontPath ||
        kind == TextEditKind::Text3DAlignment || kind == TextEditKind::Text3DFillRule;
    if (text3dProperty) {
        const EditorObject* object = workspace_.document().find_object(textEdit_.objectId);
        if (!object || !object->text3d) {
            set_status("3D text target no longer exists", true);
            textEdit_ = {};
            return;
        }
        Text3DStyle style = object->text3d->style;
        std::string propertyError;
        if (kind == TextEditKind::Text3DSize) {
            const auto value = parse_float_line(textEdit_.buffer);
            if (!value || *value < 0.001F || *value > 1000.0F) propertyError = "3D text size must be between 0.001 and 1000 meters";
            else style.emSizeMeters = *value;
        } else if (kind == TextEditKind::Text3DDepth) {
            const auto value = parse_float_line(textEdit_.buffer);
            if (!value || *value < 0.0F || *value > 100.0F) propertyError = "3D text depth must be between 0 and 100 meters";
            else style.extrusionDepthMeters = *value;
        } else if (kind == TextEditKind::Text3DLetterSpacing) {
            const auto value = parse_float_line(textEdit_.buffer);
            if (!value || *value < -2.0F || *value > 10.0F) propertyError = "3D text letter spacing must be between -2 and 10 em";
            else style.letterSpacingEm = *value;
        } else if (kind == TextEditKind::Text3DFaceMaterial) {
            const auto value = parse_uint_line(textEdit_.buffer);
            if (!value || *value == 0U) propertyError = "Face material ID must be a positive integer";
            else style.faceMaterialId = *value;
        } else if (kind == TextEditKind::Text3DSideMaterial) {
            const auto value = parse_uint_line(textEdit_.buffer);
            if (!value || *value == 0U) propertyError = "Side material ID must be a positive integer";
            else style.sideMaterialId = *value;
        } else if (kind == TextEditKind::Text3DAlignment) {
            const std::string value = lowercase(textEdit_.buffer);
            if (value == "left") style.alignment = Text3DHorizontalAlignment::Left;
            else if (value == "center" || value == "centre") style.alignment = Text3DHorizontalAlignment::Center;
            else if (value == "right") style.alignment = Text3DHorizontalAlignment::Right;
            else propertyError = "Alignment must be left, center, or right";
        } else if (kind == TextEditKind::Text3DFillRule) {
            const std::string value = lowercase(textEdit_.buffer);
            if (value == "nonzero" || value == "non-zero") style.fillRule = Text3DFillRule::NonZero;
            else if (value == "evenodd" || value == "even-odd") style.fillRule = Text3DFillRule::EvenOdd;
            else propertyError = "Fill rule must be nonzero or evenodd";
        }
        if (!propertyError.empty()) {
            set_status(propertyError, true, 6.0F);
            textEdit_ = {};
            return;
        }
        if (!text3dAuthoring_.begin_edit(projectRoot_, *object)) {
            set_status("Could not open 3D text authoring session", true);
            textEdit_ = {};
            return;
        }
        if (kind == TextEditKind::Text3DContent) text3dAuthoring_.set_text(textEdit_.buffer);
        else if (kind == TextEditKind::Text3DFontPath) text3dAuthoring_.set_font_path(textEdit_.buffer);
        else text3dAuthoring_.set_style(style);
        const bool refreshed = text3dAuthoring_.refresh();
        const CommandResult result = refreshed
            ? text3dAuthoring_.commit(workspace_)
            : CommandResult::fail(text3dAuthoring_.preview().error);
        set_status(result.success ? "Updated 3D text" : result.message, !result.success, 6.0F);
        textEdit_ = {};
        recompute_layout();
        return;
    }
    const bool gaborProperty = kind == TextEditKind::GaborDensity ||
        kind == TextEditKind::GaborTint || kind == TextEditKind::GaborEmission ||
        kind == TextEditKind::GaborAnisotropy || kind == TextEditKind::GaborShadowStrength ||
        kind == TextEditKind::GaborLodBias;
    if (gaborProperty) {
        const EditorObject* object = workspace_.document().find_object(textEdit_.objectId);
        if (!object || !object->gaborVolume) {
            set_status("Gabor volume target no longer exists", true);
            textEdit_ = {};
            return;
        }
        GaborVolumeAsset replacement = *object->gaborVolume;
        std::string propertyError;
        if (kind == TextEditKind::GaborTint) {
            const auto value = parse_float3_line(textEdit_.buffer);
            if (!value || value->x < 0.0F || value->y < 0.0F || value->z < 0.0F ||
                value->x > 16.0F || value->y > 16.0F || value->z > 16.0F)
                propertyError = "Gabor tint components must be between 0 and 16";
            else replacement.material.albedoTint = *value;
        } else {
            const auto value = parse_float_line(textEdit_.buffer);
            if (!value) propertyError = "Could not parse a numeric Gabor property";
            else if (kind == TextEditKind::GaborDensity) {
                if (*value < 0.0F || *value > 10000.0F) propertyError = "Gabor density must be between 0 and 10000";
                else replacement.material.densityMultiplier = *value;
            } else if (kind == TextEditKind::GaborEmission) {
                if (*value < 0.0F || *value > 10000.0F) propertyError = "Gabor emission must be between 0 and 10000";
                else replacement.material.emissionIntensity = *value;
            } else if (kind == TextEditKind::GaborAnisotropy) {
                if (*value < -0.99F || *value > 0.99F) propertyError = "Gabor anisotropy must be between -0.99 and 0.99";
                else replacement.material.anisotropy = *value;
            } else if (kind == TextEditKind::GaborShadowStrength) {
                if (*value < 0.0F || *value > 1.0F) propertyError = "Gabor shadow strength must be between 0 and 1";
                else replacement.material.shadowStrength = *value;
            } else if (kind == TextEditKind::GaborLodBias) {
                if (*value < -8.0F || *value > 8.0F) propertyError = "Gabor LOD bias must be between -8 and 8";
                else replacement.material.lodBias = *value;
            }
        }
        if (!propertyError.empty()) {
            set_status(propertyError, true, 6.0F);
            textEdit_ = {};
            return;
        }
        replacement.recompute_bounds_and_hash();
        GaborVolumeObjectState before{object->gaborVolume, object->sourceAsset, object->importRecipe};
        GaborVolumeObjectState after{replacement, object->sourceAsset, object->importRecipe};
        const CommandResult result = workspace_.commands().execute(
            workspace_.document(), std::make_unique<ReplaceGaborVolumeObjectCommand>(
                object->id, std::move(before), std::move(after), "Edit Gabor volume material"));
        set_status(result.success ? "Updated Gabor volume" : result.message, !result.success, 6.0F);
        textEdit_ = {};
        recompute_layout();
        return;
    }

    // Transform fields describe the primary of the selection captured when editing began.
    // A selection or transform changed by another route must never receive this old value.
    if (kind == TextEditKind::Position || kind == TextEditKind::Rotation) {
        if (textEdit_.replaceOnNextInput) { textEdit_ = {}; return; }
        const auto sameVector = [](Float3 a, Float3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
        bool stale = workspace_.selected_object() != textEdit_.objectId ||
                     workspace_.selected_objects() != textEdit_.transformSelection;
        for (const auto& change : textEdit_.transformSnapshot) {
            const auto* object = workspace_.document().find_object(change.id);
            if (!object) { stale = true; break; }
            const auto& a = object->transform.rotation; const auto& b = change.before.rotation;
            if (!sameVector(object->transform.position, change.before.position) ||
                a.x != b.x || a.y != b.y || a.z != b.z || a.w != b.w) stale = true;
            if (object->flags.locked) {
                set_status("Unlock every selected object before editing its transform", true, 8.0F);
                textEdit_ = {}; return;
            }
        }
        if (stale) {
            set_status("Selection or transform changed: reopen the field before applying a value", true, 8.0F);
            textEdit_ = {}; return;
        }
    }
    // Numeric fields: one axis (or one value) at a time, with units and relative edits.
    const NumberFieldId field = kind == TextEditKind::DuplicateSetting ? static_cast<NumberFieldId>(textEdit_.objectId)
        : kind == TextEditKind::Position ? NumberFieldId::Position
        : kind == TextEditKind::Rotation ? NumberFieldId::Rotation
        : kind == TextEditKind::VoxelSize ? NumberFieldId::VoxelSize
        : kind == TextEditKind::BrushRadius ? NumberFieldId::BrushRadius : NumberFieldId::SurfaceOffset;
    const int axis = textEdit_.axis;
    const std::string buffer = textEdit_.buffer;
    const bool untouched = textEdit_.replaceOnNextInput;
    textEdit_ = {};
    if (!untouched) (void)set_number_field(field, axis, buffer);
}

void NativeEditorController::cancel_text_edit() noexcept {
    if (textEdit_.kind == TextEditKind::HierarchyFilter) {
        // Escape clears an in-progress filter rather than merely closing the field, so it
        // reads as "show everything again", which is what a cancel gesture should mean here.
        hierarchyFilter_.clear();
        recompute_layout();
    }
    if (textEdit_.kind == TextEditKind::AssetSearch) {
        assetBrowserState_.query.text.clear();
        assetBrowserState_.firstVisible = 0U;
        recompute_layout();
    }
    textEdit_ = {};
}

void NativeEditorController::key_up(std::string_view key, bool control, bool, bool alt) {
    if (!control && !alt && synthPanel_.open()) (void)synthPanel_.key_up(key, audioMixer_.synthesizer());
    const std::string released = lowercase(key);
    if (spriteLevelPlaying_ && !control && !alt) {
        if ((released == "left" || released == "a") && spriteLevelInput_.moveX < 0.0F)
            spriteLevelInput_.moveX = 0.0F;
        if ((released == "right" || released == "d") && spriteLevelInput_.moveX > 0.0F)
            spriteLevelInput_.moveX = 0.0F;
        if (released == "space" || released == "up" || released == "w")
            spriteLevelInput_.jumpHeld = false;
    }
    route_play_key(released, false);
    std::erase_if(heldShortcutGestures_, [&](const ShortcutGesture& gesture) {
        return gesture.device == ShortcutDevice::Keyboard && lowercase(gesture.input) == released;
    });
}

void NativeEditorController::text_input(std::string_view text) {
    if (longJob_) return;
    if (voxel_authoring_active()) return; // Numeric authoring input is handled once in key_down.
    if (polygonPreviewBefore_) return; // Numeric preview input is handled once in key_down.
    if (importOptions_.open) {
        // Typing a number into the Voxel size or Scale row; Enter applies it.
        if (importOptions_.selected < 2)
            for (const char c : text)
                if ((std::isdigit(static_cast<unsigned char>(c)) || c == '.') && importOptions_.typed.size() < 12) importOptions_.typed += c;
        return;
    }
    if (shortcutPanel_.open && !shortcutPanel_.capturing && !text.empty()) {
        shortcutPanel_.searchQuery.append(text);
        shortcutPanel_.selectedRow = 0U;
        return;
    }
    if (settingsPanel_.open && !text.empty()) {
        if (settingsPanel_.valueEditing) settingsPanel_.append_value_text(text);
        else {
            settingsPanel_.searchQuery.append(text);
            settingsPanel_.selectedRow = 0U;
        }
        return;
    }
    if (filePicker_.open && !text.empty()) {
        filePicker_.query.append(text);
        filePicker_.selected = 0U;
        filePicker_.confirmReplace.reset();
        if (filePicker_.messageIsError) { filePicker_.message.clear(); filePicker_.messageIsError = false; }
        return;
    }
    if (commandPaletteOpen_ && !text.empty()) {
        commandPaletteQuery_.append(text);
        commandPaletteSelection_ = 0U;
        return;
    }
    if (controlRigPanel_.open() && controlRigPanel_.text_input(text)) return;
    if (audioEventPanel_.open() && audioEventPanel_.text_input(text)) return;
    if (textEdit_.kind == TextEditKind::Inactive || text.empty()) return;
    if (textEdit_.replaceOnNextInput) {
        textEdit_.buffer.clear();
        textEdit_.replaceOnNextInput = false;
    }
    textEdit_.buffer.append(text);
    if (textEdit_.kind == TextEditKind::HierarchyFilter) {
        hierarchyFilter_ = textEdit_.buffer;
        hierarchyScroll_ = 0;
        if (std::string error; !hierarchyFilter_.ends_with(':') && looks_like_selection_query(hierarchyFilter_) &&
                               !parse_selection_query(hierarchyFilter_, &error))
            set_status(error, true);
        recompute_layout();
    } else if (textEdit_.kind == TextEditKind::AssetSearch) {
        assetBrowserState_.query.text = textEdit_.buffer;
        assetBrowserState_.firstVisible = 0U;
        recompute_layout();
    }
}

void NativeEditorController::close_context_menu() noexcept { contextMenu_ = {}; }

void NativeEditorController::open_context_menu(int x, int y, std::optional<EditorObjectId> target, bool fromHierarchy) {
    contextMenu_ = {};
    contextMenu_.open = true;
    contextMenu_.x = x;
    contextMenu_.y = y;
    if (target) {
        contextMenu_.items.push_back({"edit.duplicate", "Duplicate"});
        if (fromHierarchy) contextMenu_.items.push_back({"edit.rename", "Rename"});
        contextMenu_.items.push_back({"view.frame", "Frame Selection"});
        contextMenu_.items.push_back({"hierarchy.reveal_selection", "Reveal in Hierarchy"});
        if (workspace_.selection_count() >= 2U && (!target || workspace_.is_selected(*target)) &&
            voxel_boolean_disabled_reason().empty()) {
            contextMenu_.items.push_back({"voxel.boolean_union", "Boolean Union"});
            contextMenu_.items.push_back({"voxel.boolean_difference", "Boolean Difference"});
            contextMenu_.items.push_back({"voxel.boolean_intersection", "Boolean Intersection"});
        }
        contextMenu_.items.push_back({"edit.delete", "Delete"});
    } else {
        if (!clipboard_.empty()) contextMenu_.items.push_back({"edit.paste", "Paste"});
        contextMenu_.items.push_back({"create.empty", "Create Empty Object"});
        contextMenu_.items.push_back({"create.voxel", "Create Voxel Object"});
        contextMenu_.items.push_back({"create.text3d", "Create 3D Text"});
        contextMenu_.items.push_back({"create.gabor_empty", "Create Gabor Volume"});
    }
    // Right-click on an unselected object selects it first, so Duplicate/Delete/Frame act on
    // the object under the cursor rather than whatever was selected before the right-click.
    if (target && !workspace_.is_selected(*target)) workspace_.select_object(*target);
    const int itemHeight = static_cast<int>(24.0F * kLogicalLayoutScale);
    constexpr int popupWidth = 190;
    const int popupHeight = itemHeight * static_cast<int>(contextMenu_.items.size());
    contextMenu_.x = std::clamp(x, 0, std::max(0, width_ - popupWidth));
    contextMenu_.y = std::clamp(y, layout_.menuBar.height, std::max(layout_.menuBar.height, height_ - popupHeight));
    contextMenu_.itemRects.clear();
    for (std::size_t i = 0; i < contextMenu_.items.size(); ++i) {
        contextMenu_.itemRects.push_back({contextMenu_.x, contextMenu_.y + static_cast<int>(i) * itemHeight,
                                          popupWidth, itemHeight});
    }
}

void NativeEditorController::open_pick_list(int x, int y, const std::vector<EditorPickResult>& hits) {
    contextMenu_ = {};
    contextMenu_.open = true;
    const int itemHeight = static_cast<int>(24.0F * kLogicalLayoutScale);
    const int popupWidth = 380;
    contextMenu_.width = popupWidth;
    const std::size_t count = std::min<std::size_t>(hits.size(), 16U);
    contextMenu_.x = std::clamp(x, 0, std::max(0, width_ - popupWidth));
    contextMenu_.y = std::clamp(y, layout_.menuBar.height,
        std::max(layout_.menuBar.height, height_ - itemHeight * static_cast<int>(count)));
    for (std::size_t index = 0; index < count; ++index) {
        const EditorObjectId id = hits[index].objectId;
        const EditorObject* object = workspace_.document().find_object(id);
        if (!object) continue;
        // "Name  (kind)  Parent / Name", nearest first: enough to tell stacked objects apart.
        std::string path;
        for (auto parent = object->parent; parent;) {
            const EditorObject* ancestor = workspace_.document().find_object(*parent);
            if (!ancestor) break;
            path = ancestor->name + " / " + path;
            parent = ancestor->parent;
        }
        std::string kind;
        if (object->text3d) kind = "3D text";
        else if (object->gaborVolume) kind = "Gabor volume";
        else if (object->subdivision && object->polygon) kind = "subdivision, " + std::to_string(object->polygon->indices.size() / 3) + " tris";
        else if (object->polygon) kind = "mesh, " + std::to_string(object->polygon->indices.size() / 3) + " tris";
        else if (object->voxels && object->voxels->occupied_voxel_count() > 0) kind = "voxels, " + std::to_string(object->voxels->occupied_voxel_count());
        else kind = "group";
        std::string label = object->name + "  (" + kind + ")";
        if (!path.empty()) label += "  " + path + object->name;
        contextMenu_.items.push_back({"selection.pick." + std::to_string(id), std::move(label)});
        contextMenu_.itemRects.push_back({contextMenu_.x,
            contextMenu_.y + static_cast<int>(contextMenu_.itemRects.size()) * itemHeight,
            popupWidth, itemHeight});
    }
}

std::optional<EditorObjectId> NativeEditorController::hierarchy_row_object_at(int x, int y) const {
    const auto order = hierarchy_order();
    for (std::size_t index = 0; index < layout_.hierarchyRows.size() && index < order.size(); ++index) {
        if (layout_.hierarchyRows[index].contains(x, y)) return order[index];
    }
    return std::nullopt;
}

bool NativeEditorController::pick_walk(WalkDirection direction) {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) { set_status("Stop Play before changing the selection", true); return false; }
    if (gizmoDragging_ || longJob_) { set_status("Finish the active edit first", true); return false; }
    if (selectionDomain_ == SelectionDomain::Polygons) return polygon_walk(direction);
    if (selectionDomain_ == SelectionDomain::Voxels) {
        if (direction == WalkDirection::Up) return dispatch_action("voxel.select_grow");
        if (direction == WalkDirection::Down) return dispatch_action("voxel.select_shrink");
        set_status("Voxels: Up grows and Down shrinks the selection; Left/Right walk objects (press M)", true);
        return false;
    }
    const EditorDocument& document = workspace_.document();
    const auto primary = workspace_.selected_object();
    if (!primary || !document.find_object(*primary)) { set_status("Select an object to walk from", true); return false; }
    const auto walkable = [&](EditorObjectId id) {
        const EditorObject* object = document.find_object(id);
        return object && object->flags.visible && !is_isolated_out(id);
    };
    std::optional<EditorObjectId> target;
    const EditorObject* current = document.find_object(*primary);
    switch (direction) {
    case WalkDirection::Up:
        for (auto parent = current->parent; parent && !target;) {
            if (walkable(*parent)) target = *parent;
            const EditorObject* ancestor = document.find_object(*parent);
            parent = ancestor ? ancestor->parent : std::nullopt;
        }
        if (!target) { set_status(current->name + " has no parent", true); return false; }
        break;
    case WalkDirection::Down:
        for (const EditorObjectId child : document.children_of(*primary))
            if (walkable(child)) { target = child; break; }
        if (!target) { set_status(current->name + " has no children", true); return false; }
        break;
    case WalkDirection::Left:
    case WalkDirection::Right: {
        std::vector<EditorObjectId> siblings = current->parent ? document.children_of(*current->parent) : document.root_objects();
        std::erase_if(siblings, [&](EditorObjectId id) { return id != *primary && !walkable(id); });
        const auto at = std::find(siblings.begin(), siblings.end(), *primary);
        if (siblings.size() < 2 || at == siblings.end()) { set_status(current->name + " has no siblings", true); return false; }
        const std::size_t index = static_cast<std::size_t>(at - siblings.begin());
        target = siblings[(index + (direction == WalkDirection::Right ? 1U : siblings.size() - 1U)) % siblings.size()];
        break;
    }
    }
    workspace_.select_object(*target);
    recompute_layout();
    refresh_menu_state();
    const EditorObject* reached = document.find_object(*target);
    set_status("Selected " + reached->name + "  (arrows walk: Up parent, Down child, Left/Right siblings)");
    return true;
}

bool NativeEditorController::mirror_selection(int axis, bool copy) {
    const char* axisName = axis == 0 ? "X" : axis == 1 ? "Y" : "Z";
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) { set_status("Stop Play or Simulate before mirroring", true); return false; }
    if (longJob_) { set_status(long_job_busy_reason(), true); return false; }
    if (selectionDomain_ != SelectionDomain::Objects) {
        set_status(selectionDomain_ == SelectionDomain::Voxels ? "Mirror works on whole objects: press M to return to objects"
                                                               : "Mirror works on whole objects here; return to object selection first", true);
        return false;
    }
    EditorDocument& document = workspace_.document();
    // Roots only: a selected child of a selected parent mirrors with its parent.
    const std::set<EditorObjectId> selected(workspace_.selected_objects().begin(), workspace_.selected_objects().end());
    std::vector<EditorObjectId> roots;
    for (const EditorObjectId id : workspace_.selected_objects()) {
        const EditorObject* object = document.find_object(id);
        if (!object) continue;
        bool ancestorSelected = false;
        for (auto parent = object->parent; parent && !ancestorSelected;) {
            ancestorSelected = selected.contains(*parent);
            const EditorObject* ancestor = document.find_object(*parent);
            parent = ancestor ? ancestor->parent : std::nullopt;
        }
        if (!ancestorSelected && std::find(roots.begin(), roots.end(), id) == roots.end()) roots.push_back(id);
    }
    if (roots.empty()) { set_status("Select the objects to mirror first", true); return false; }
    const std::vector<EditorObjectId> subtree = collect_editor_object_subtree_ids(document, roots);
    for (const EditorObjectId id : roots) {
        const EditorObject* object = document.find_object(id);
        if (!object->flags.visible || is_isolated_out(id)) { set_status(object->name + " is hidden or isolated out of view", true); return false; }
    }
    for (const EditorObjectId id : subtree) {
        const EditorObject* object = document.find_object(id);
        if (object->text3d || object->gaborVolume) {
            set_status("Mirror does not flip " + std::string(object->text3d ? "3D text" : "Gabor volumes") + " (" + object->name + ")", true);
            return false;
        }
        if (object->prefabLink) { set_status(object->name + " is part of a prefab instance; unpack it or mirror the prefab", true); return false; }
        if (!copy && object->flags.locked) { set_status("Unlock " + object->name + " before mirroring it", true); return false; }
    }
    close_previews_for_voxel_edit("Mirror");
    const Float3 point = selection_pivot();
    const auto beforeSelection = capture_selection_snapshot();
    // Mirrored pose of every object first, so children can be placed relative to their mirrored parent.
    std::map<EditorObjectId, MirroredPose> poses;
    for (const EditorObjectId id : subtree) {
        const EditorObject* object = document.find_object(id);
        poses.emplace(id, mirror_pose(document.world_transform(id).value_or(object->transform), axis, point));
    }
    std::string error;
    const std::string label = std::string(copy ? "Mirror copy " : "Mirror ") + axisName;
    auto compound = std::make_unique<CompoundCommand>(label);
    std::vector<EditorObjectId> rootCopies;
    if (copy) {
        std::map<EditorObjectId, EditorObjectId> idMap;
        for (const EditorObjectId id : subtree) idMap.emplace(id, document.allocate_object_id());
        for (const EditorObjectId id : subtree) {
            const EditorObject* source = document.find_object(id);
            const MirroredPose& pose = poses.at(id);
            EditorObject mirrored = clone_editor_object(*source);
            mirrored.id = idMap.at(id);
            const bool root = std::find(roots.begin(), roots.end(), id) != roots.end();
            if (root) { mirrored.name = source->name + " (mirror)"; rootCopies.push_back(mirrored.id); }
            mirrored.parent = source->parent && idMap.contains(*source->parent) ? std::optional(idMap.at(*source->parent)) : source->parent;
            mirrored.transform = pose.transform;
            if (mirrored.attachment && source->parent && poses.contains(*source->parent))
                mirrored.attachment->localTransform = relative_rigid_transform(poses.at(*source->parent).transform, pose.transform);
            if (source->polygon) {
                mirrored.polygon->objectId = mirrored.id;  // before the refresh: the content hash covers it
                const PolygonTopologyLineage lineage = mirror_polygon_lineage(*source->polygon, source->subdivision ? &*source->subdivision : nullptr);
                if (!mirror_polygon_mesh(*mirrored.polygon, pose.localAxis, mirrored.subdivision ? &*mirrored.subdivision : nullptr,
                                         mirrored.modelingTopology ? &*mirrored.modelingTopology : nullptr, &error)) {
                    set_status("Mirror failed on " + source->name + ": " + error, true);
                    return false;
                }
                mirrored.subdivisionCache.reset();
                const auto record = remap_polygon_uv_authoring_record(*source, *mirrored.polygon, lineage, &error);
                if (!record) { set_status("Mirror refused on " + source->name + ": " + error, true); return false; }
                if (!record->empty())
                    if (Component* component = find_component_by_type(std::span<Component>(mirrored.components), kUvAuthoringComponent))
                        component->properties["state"] = ComponentValue{*record};
            } else if (source->voxels && source->voxels->occupied_voxel_count() > 0) {
                const VoxelObjectState state = mirror_voxel_state(capture_voxel_object_state(*source), pose.localAxis);
                auto voxels = std::make_unique<VoxelObject>(mirrored.id);
                for (const SparseVoxelStateEntry& entry : state.voxels) (void)voxels->set_voxel(entry.voxel, entry.material);
                mirrored.voxels = std::move(voxels);
                mirrored.anchors = std::set<Int3>(state.anchors.begin(), state.anchors.end());
            }
            compound->add(std::make_unique<AddObjectCommand>(std::move(mirrored), label));
        }
    } else {
        std::vector<ObjectTransformChange> transforms;
        for (const EditorObjectId id : subtree) {
            const EditorObject* object = document.find_object(id);
            transforms.push_back({id, document.world_transform(id).value_or(object->transform), poses.at(id).transform});
        }
        compound->add(std::make_unique<TransformObjectsCommand>(std::move(transforms), label));
        for (const EditorObjectId id : subtree) {
            const EditorObject* object = document.find_object(id);
            const int e = poses.at(id).localAxis;
            if (object->polygon) {
                CookedPolygonAsset mesh = *object->polygon;
                std::optional<SubdivisionSettings> subdivision = object->subdivision;
                std::optional<ModelingTopology> topology = object->modelingTopology;
                const PolygonTopologyLineage lineage = mirror_polygon_lineage(mesh, subdivision ? &*subdivision : nullptr);
                if (!mirror_polygon_mesh(mesh, e, subdivision ? &*subdivision : nullptr, topology ? &*topology : nullptr, &error)) {
                    set_status("Mirror failed on " + object->name + ": " + error, true);
                    return false;
                }
                // Saved UV seams and pins follow the swapped corners.
                const auto record = remap_polygon_uv_authoring_record(*object, mesh, lineage, &error);
                if (!record) { set_status("Mirror refused on " + object->name + ": " + error, true); return false; }
                compound->add(std::make_unique<ReplacePolygonCommand>(id, *object->polygon, std::move(mesh), label, 0,
                                                                      std::move(subdivision), false, std::move(topology)));
                if (!record->empty())
                    if (const Component* component = find_component_by_type(std::span<const Component>(object->components), kUvAuthoringComponent)) {
                        const auto at = component->properties.find("state");
                        compound->add(std::make_unique<SetComponentPropertyCommand>(id, component->id, "state",
                            at == component->properties.end() ? std::optional<ComponentValue>{} : at->second, ComponentValue{*record}, label));
                    }
            } else if (object->voxels && object->voxels->occupied_voxel_count() > 0) {
                VoxelObjectState before = capture_voxel_object_state(*object);
                VoxelObjectState after = mirror_voxel_state(before, e);
                compound->add(std::make_unique<ReplaceVoxelObjectStateCommand>(id, std::move(before), std::move(after), label));
            }
        }
    }
    const CommandResult result = workspace_.commands().execute(document, std::move(compound));
    if (!result.success) { set_status(label + " failed: " + result.message, true); return false; }
    if (copy) {
        workspace_.clear_selection();
        for (const EditorObjectId id : rootCopies) workspace_.add_to_selection(id);
        if (!viewportSettings_.isolatedObjects.empty())
            for (const auto& [id, object] : document.objects()) { (void)object; if (!poses.contains(id)) viewportSettings_.isolatedObjects.insert(id); }
        workspace_.commands().set_last_selection_transition(beforeSelection, capture_selection_snapshot());
    }
    recompute_layout();
    refresh_menu_state();
    char where[96];
    std::snprintf(where, sizeof where, "%s = %.3g", axisName, static_cast<double>(axis == 0 ? point.x : axis == 1 ? point.y : point.z));
    set_status(std::string(copy ? "Mirrored copy of " : "Mirrored ") + std::to_string(subtree.size()) +
               (subtree.size() == 1 ? " object" : " objects") + " across " + where + " (the pivot); Undo restores");
    return true;
}

CommandResult NativeEditorController::duplicate_objects(const std::vector<EditorObjectId>& ids, Float3 offset) {
    if (ids.empty()) return CommandResult::fail("nothing selected to duplicate");
    const std::vector<EditorObjectId> closure = collect_editor_object_subtree_ids(workspace_.document(), ids);
    if (closure.empty()) return CommandResult::fail("selected objects no longer exist");

    std::map<EditorObjectId, EditorObjectId> idMap;
    for (const EditorObjectId oldId : closure) idMap.emplace(oldId, workspace_.document().allocate_object_id());

    auto compound = std::make_unique<CompoundCommand>(closure.size() == 1 ? "Duplicate object" : "Duplicate objects");
    std::vector<EditorObjectId> newIds;
    newIds.reserve(closure.size());
    for (const EditorObjectId oldId : closure) {
        const EditorObject* source = workspace_.document().find_object(oldId);
        if (!source) return CommandResult::fail("duplicate source no longer exists");
        EditorObject duplicate = clone_editor_object(*source);
        duplicate.id = idMap.at(oldId);
        duplicate.parent = source->parent && idMap.contains(*source->parent)
            ? std::optional<EditorObjectId>(idMap.at(*source->parent))
            : source->parent;
        duplicate.transform.position = add(duplicate.transform.position, offset);
        newIds.push_back(duplicate.id);
        compound->add(std::make_unique<AddObjectCommand>(std::move(duplicate), "Duplicate object"));
    }
    const CommandResult result = workspace_.commands().execute(workspace_.document(), std::move(compound));
    if (result.success) {
        workspace_.clear_selection();
        for (const EditorObjectId oldRoot : ids) {
            const auto mapped = idMap.find(oldRoot);
            if (mapped != idMap.end()) workspace_.add_to_selection(mapped->second);
        }
        recompute_layout();
    }
    return result;
}

void NativeEditorController::update_hover(int x, int y) {
    if (!layout_.viewport.contains(x, y)) {
        hoverPick_.reset();
        return;
    }
    hoverPick_ = pick_editor_document(workspace_.document(), make_viewport_ray(camera_, layout_.viewport,
                                                                               static_cast<float>(x), static_cast<float>(y)),
                                      10000.0F, &viewportSettings_.isolatedObjects);
}

bool NativeEditorController::navigation_pointer_active() const noexcept {
    return focusRegion_ == EditorFocusRegion::Viewport && workspace_.mode() == EditorMode::Edit &&
        !settingsPanel_.open && !shortcutPanel_.open && !commandPaletteOpen_ &&
        !pending_destructive_confirmation() && !openMenu_ && !contextMenu_.open &&
        (activePointerCommand_ == "viewport.look" || activePointerCommand_ == "viewport.orbit" ||
         activePointerCommand_ == "viewport.pan" || activePointerCommand_ == "viewport.dolly");
}

void NativeEditorController::pointer_relative(float deltaX, float deltaY) {
    if (navigation_pointer_active() && std::isfinite(deltaX) && std::isfinite(deltaY))
        navigate_pointer(deltaX, deltaY);
}

void NativeEditorController::navigate_pointer(float deltaX, float deltaY) {
    if (activePointerCommand_ == "viewport.look" || activePointerCommand_ == "viewport.orbit") {
        const SettingValue sensitivityValue = workspace_.settings().value("camera.mouse_sensitivity");
        const SettingValue orbitValue = workspace_.settings().value("camera.orbit_sensitivity");
        const SettingValue invertValue = workspace_.settings().value("camera.invert_y");
        const float sensitivity = std::get_if<double>(&sensitivityValue)
            ? static_cast<float>(std::get<double>(sensitivityValue)) : workspace_.preferences().mouseSensitivity;
        const float orbitScale = std::get_if<double>(&orbitValue)
            ? static_cast<float>(std::get<double>(orbitValue)) : 1.0F;
        const bool invertY = std::get_if<bool>(&invertValue) && std::get<bool>(invertValue);
        const float adjustedY = static_cast<float>(deltaY) * (invertY ? -1.0F : 1.0F);
        if (cameraInputSmoothing_ > 0.0F) {
            const float scale = 0.006F * sensitivity * (activePointerCommand_ == "viewport.orbit" ? orbitScale : 1.0F);
            pendingCameraLook_[0] += static_cast<float>(deltaX) * scale;
            pendingCameraLook_[1] += adjustedY * scale;
        } else if (activePointerCommand_ == "viewport.look")
            look_camera(camera_, static_cast<float>(deltaX), adjustedY, 0.006F * sensitivity);
        else
            orbit_camera(camera_, static_cast<float>(deltaX), adjustedY, 0.006F * sensitivity * orbitScale);
    } else if (activePointerCommand_ == "viewport.pan") {
        const SettingValue panValue = workspace_.settings().value("camera.pan_sensitivity");
        const float panScale = std::get_if<double>(&panValue)
            ? static_cast<float>(std::get<double>(panValue)) : 1.0F;
        pan_camera(camera_, static_cast<float>(deltaX) * panScale, static_cast<float>(deltaY) * panScale, layout_.viewport);
    } else if (activePointerCommand_ == "viewport.dolly") {
        const SettingValue zoomValue = workspace_.settings().value("camera.zoom_sensitivity");
        const float sensitivity = std::get_if<double>(&zoomValue)
            ? static_cast<float>(std::get<double>(zoomValue)) : 1.0F;
        zoom_camera(camera_, static_cast<float>(-deltaY) * 0.08F * sensitivity);
    }
}

void NativeEditorController::pointer_move(int x, int y, std::uint32_t modifiers) {
    if (numberScrub_.armed) {
        hoverX_ = x; hoverY_ = y;
        update_number_scrub(x, modifiers);
        lastPointerX_ = x; lastPointerY_ = y;
        return;
    }
    if (polygonLoopCutDragging_) {
        polygonPreviewTyped_.clear();
        polygonPreviewSettings_.amount=std::clamp(polygonLoopCutStartFraction_+
            (float(x)-float(polygonLoopCutStartX_))/float((std::max)(120,layout_.viewport.width/2)),.001F,.999F);
        update_polygon_preview(); lastPointerX_=x; lastPointerY_=y; return;
    }
    if(!longJob_ && textEdit_.kind==TextEditKind::Inactive && mesh_authoring_pointer_move(x,y,modifiers)){lastPointerX_=x;lastPointerY_=y;return;}
    hoverX_ = x;
    hoverY_ = y;
    if (assetDrag_.armed) {
        update_asset_drag(x, y);
        lastPointerX_ = x; lastPointerY_ = y;
        return;
    }
    if (sliceDrag_) {
        update_slice_pointer(x, y, modifiers);
        lastPointerX_ = x; lastPointerY_ = y;
        return;
    }
    if (longJob_ || importOptions_.open) {
        lastPointerX_ = x;
        lastPointerY_ = y;
        return;
    }
    if (filePicker_.open) {
        const NativeFilePickerLayout picker = file_picker_layout();
        for (std::size_t visible = 0; visible < picker.rows.size(); ++visible)
            if (picker.rows[visible].contains(x, y)) { filePicker_.selected = picker.firstVisibleRow + visible; break; }
        lastPointerX_ = x;
        lastPointerY_ = y;
        return;
    }
    if (commandPaletteOpen_) {
        const NativeCommandPaletteLayout palette = command_palette_layout();
        const auto results = command_palette_results(64);
        for (std::size_t visible = 0; visible < palette.rows.size(); ++visible) {
            if (!palette.rows[visible].contains(x, y)) continue;
            const std::size_t rowIndex = palette.firstVisibleRow + visible;
            if (rowIndex < results.size()) commandPaletteSelection_ = rowIndex;
            break;
        }
        lastPointerX_ = x;
        lastPointerY_ = y;
        return;
    }
    if (settingsPanel_.open) { lastPointerX_ = x; lastPointerY_ = y; return; }
    if (tileWorldEditorOpen_) {
        std::string error;
        const TileVec2 point{static_cast<float>(x), static_cast<float>(y)};
        (void)tileWorldEditor_.palette_pointer_move(point);
        (void)tileWorldEditor_.canvas_pointer_move(point, tile_world_canvas_viewport(), &error);
        if (!error.empty()) set_status(error, true);
        lastPointerX_ = x; lastPointerY_ = y; return;
    }
    if (spriteAnimationGraphOpen_) {
        (void)spriteAnimationGraph_.pointer_move({static_cast<float>(x), static_cast<float>(y)},
                                                 sprite_animation_graph_viewport());
        lastPointerX_ = x; lastPointerY_ = y; return;
    }
    if (spriteAuthoringPanel_.open() && spriteAuthoringPanel_.pointer_move(x, y, modifiers)) {
        lastPointerX_ = x; lastPointerY_ = y; return;
    }
    if (controlRigPanel_.open() && controlRigPanel_.pointer_move(x, y, modifiers)) {
        lastPointerX_ = x; lastPointerY_ = y; return;
    }
    if (chiptunePanel_.open() && chiptunePanel_.pointer_move(x, y)) return;
    if (synthPanel_.open() && synthPanel_.pointer_move(x, y, audioMixer_.synthesizer())) return;
    if (audioEventPanel_.open() && audioEventPanel_.pointer_move(x, y)) {
        lastPointerX_ = x;
        lastPointerY_ = y;
        return;
    }
    if (openMenu_) {
        if (layout_.menuBar.contains(x, y)) {
            if (const auto menu = menu_name_at(x); menu && *menu != *openMenu_)
                open_top_level_menu(*menu);
        }
        menuHoveredAction_.reset();
        const NativeMenuPopupLayout popup = menu_popup_layout();
        for (std::size_t visible = 0; visible < popup.rows.size(); ++visible) {
            if (popup.rows[visible].contains(x, y)) {
                menuHoveredAction_ = popup.firstVisibleAction + visible;
                break;
            }
        }
        lastPointerX_ = x;
        lastPointerY_ = y;
        return;
    }
    if (contextMenu_.open) {
        const auto previous = contextMenu_.hoveredItem;
        contextMenu_.hoveredItem.reset();
        for (std::size_t index = 0; index < contextMenu_.itemRects.size(); ++index) {
            if (contextMenu_.itemRects[index].contains(x, y)) {
                contextMenu_.hoveredItem = index;
                break;
            }
        }
        if (contextMenu_.hoveredItem != previous && contextMenu_.hoveredItem &&
            contextMenu_.items[*contextMenu_.hoveredItem].actionId.starts_with("selection.pick."))
            set_status(contextMenu_.items[*contextMenu_.hoveredItem].label, false, 6.0F);
        lastPointerX_ = x;
        lastPointerY_ = y;
        return;
    }
    const int deltaX = x - lastPointerX_;
    const int deltaY = y - lastPointerY_;
    if (placementMode_ != PlacementTarget::NoTarget && layout_.viewport.contains(x, y)) {
        update_placement_preview(x, y);
    } else if (hierarchyDrag_.sourceId != 0) {
        // Promote a pending hierarchy-row press into an active drag once the pointer has
        // moved enough that this clearly isn't just a click.
        const int distanceSquared = (x - pointerDownX_) * (x - pointerDownX_) + (y - pointerDownY_) * (y - pointerDownY_);
        if (!hierarchyDrag_.active && distanceSquared > 25) hierarchyDrag_.active = true;
        if (hierarchyDrag_.active) hierarchyDrag_.hoverTarget = hierarchy_row_object_at(x, y);
    } else if (voxelMoveAxis_ != 0) {
        // Project the pointer travel onto the axis on screen, then snap to whole voxels.
        const EditorObject* object = workspace_.document().find_object(voxelSelectionObject_);
        for (const GizmoScreenAxis& axis : gizmo_axes()) {
            if (axis.axis != voxelMoveAxis_ || !object || !axis.start.visible || !axis.end.visible) continue;
            const float sx = axis.end.x - axis.start.x;
            const float sy = axis.end.y - axis.start.y;
            const float pixels = std::sqrt(sx * sx + sy * sy);
            if (pixels < 1.0F) break;
            const float along = (static_cast<float>(x - voxelMoveStartX_) * sx + static_cast<float>(y - voxelMoveStartY_) * sy) / pixels;
            const float worldPerPixel = std::max(0.5F, length(subtract(camera_.position, voxel_selection_centre())) * 0.12F) / pixels;
            voxelMoveSteps_ = static_cast<int>(std::lround(along * worldPerPixel / (object->voxelSizeMeters * (voxelMoveAxis_ == 1 ? object->voxelScale.x : voxelMoveAxis_ == 2 ? object->voxelScale.y : object->voxelScale.z))));
            set_status("Move voxels: " + std::to_string(voxelMoveSteps_) + " voxel(s) along " +
                       std::string(voxelMoveAxis_ == 1 ? "X" : voxelMoveAxis_ == 2 ? "Y" : "Z") + " (release applies, Esc cancels)", false, 2.0F);
            break;
        }
    } else if (paintSelecting_) {
        paint_select_at(x, y);
    } else if (voxelBrushSelecting_) {
        voxel_brush_select_at(x, y);
    } else if (marquee_.active) {
        marquee_.currentX = x;
        marquee_.currentY = y;
        if (!lassoPath_.empty()) extend_lasso(x, y);
    } else if (navigation_pointer_active()) {
        navigate_pointer(static_cast<float>(deltaX), static_cast<float>(deltaY));
    } else if (gizmoDragging_) {
        update_gizmo_drag(x, y, modifiers);
    } else if (voxelStrokeActive_) {
        continue_voxel_stroke(x, y);
    } else if (brushResizing_) {
        set_scatter_brush_radius(scatterSettings_.brushRadiusMeters * std::pow(1.01F, static_cast<float>(x - brushResizeLastX_)));
        brushResizeLastX_ = x;
    } else if (brushStroking_) {
        update_brush_cursor(x, y);
        if (brushCursor_ && (!brushLastDab_ || length(subtract(*brushCursor_, *brushLastDab_)) >= scatterSettings_.brushRadiusMeters * 0.35F))
            brush_dab();
    } else {
        update_hover(x, y);
        if (activeTool_ == EditorToolId::ScatterBrush) update_brush_cursor(x, y);
    }
    (void)modifiers;
    lastPointerX_ = x;
    lastPointerY_ = y;
}

void NativeEditorController::pointer_down(PointerButton button, int x, int y, std::uint32_t modifiers) {
    if (!longJob_ && voxel_authoring_pointer_down(button,x,y,modifiers)) return;
    if(!longJob_ && !pending_destructive_confirmation() && textEdit_.kind==TextEditKind::Inactive &&
       !settingsPanel_.open && !commandPaletteOpen_ && mesh_authoring_pointer_down(button,x,y,modifiers)){refresh_menu_state();return;}
    if (sliceDrag_) finish_slice_pointer(true);
    reset_camera_navigation();
    lastPointerX_ = x;
    lastPointerY_ = y;
    pointerDownX_ = x;
    pointerDownY_ = y;
    if (playSession_.active()) {
        const std::string input = pointer_input_name(button);
        if (!input.empty()) playSession_.set_action_pressed(input, true);
        if (button == PointerButton::Primary) playSession_.set_action_pressed("fire", true);
        if (button == PointerButton::Secondary) playSession_.set_action_pressed("aim", true);
    }
    if (pending_destructive_confirmation()) return;
    if (longJob_) {
        if (button == PointerButton::Primary && long_job_layout().cancelButton.contains(x, y)) (void)cancel_long_job();
        else set_status(long_job_busy_reason(), true, 3.0F);
        return;
    }
    if (importOptions_.open) {
        if (button != PointerButton::Primary) return;
        const NativeImportOptionsLayout panel = import_options_layout();
        if (panel.importButton.contains(x, y)) { (void)accept_import_options(); return; }
        if (panel.cancelButton.contains(x, y) || !panel.panel.contains(x, y)) {
            close_import_options(importOptions_.reimport ? "Reimport cancelled; the scene is unchanged"
                                                         : "Import cancelled; the scene is unchanged");
            return;
        }
        for (std::size_t i = 0; i < panel.rows.size(); ++i) {
            if (!panel.rows[i].contains(x, y)) continue;
            if (importOptions_.selected != panel.rowIndex[i]) importOptions_.typed.clear();
            importOptions_.selected = panel.rowIndex[i];
            if (panel.rowIndex[i] >= 2) nudge_import_option(panel.rowIndex[i], 1);  // Fill cycles, a mesh toggles
            return;
        }
        return;
    }
    if (spritePixelArtOpen_) {
        const UiRect canvas = sprite_pixel_art_canvas();
        if (!canvas.contains(x, y)) return;
        const auto& document = spritePixelArt_.document();
        const int scale = std::max(1, std::min(canvas.width / static_cast<int>(document.width),
                                               canvas.height / static_cast<int>(document.height)));
        const int imageWidth = static_cast<int>(document.width) * scale;
        const int imageHeight = static_cast<int>(document.height) * scale;
        const int originX = canvas.x + (canvas.width - imageWidth) / 2;
        const int originY = canvas.y + (canvas.height - imageHeight) / 2;
        const int px = (x - originX) / scale;
        const int py = static_cast<int>(document.height) - 1 - (y - originY) / scale;
        std::string error;
        bool success = false;
        if (button == PointerButton::Secondary || spritePixelArt_.tool() == SpritePixelTool::Eraser)
            success = spritePixelArt_.erase(px, py, &error);
        else if (spritePixelArt_.tool() == SpritePixelTool::Fill)
            success = spritePixelArt_.flood_fill(px, py, &error);
        else success = spritePixelArt_.paint(px, py, &error);
        if (!success && !error.empty()) set_status(error, true);
        spritePixelPainting_ = success && button == PointerButton::Primary && spritePixelArt_.tool() == SpritePixelTool::Pencil;
        spritePixelLastX_ = px; spritePixelLastY_ = py;
        return;
    }
    if (spriteRig2DOpen_) {
        const UiRect canvas = sprite_rig2d_canvas();
        if (!canvas.contains(x, y)) return;
        if (!spriteRig2D_.asset().constraints.empty()) {
            const float localX = static_cast<float>(x - (canvas.x + canvas.width / 2));
            const float localY = static_cast<float>((canvas.y + canvas.height / 2) - y);
            std::string error;
            if (!spriteRig2D_.set_ik_target(spriteRig2D_.asset().constraints.front().id,
                                             {localX, localY}, 1.0F, &error)) set_status(error, true);
        }
        return;
    }
    if (tileWorldEditorOpen_) {
        if (button != PointerButton::Primary && button != PointerButton::Auxiliary &&
            button != PointerButton::Secondary) return;
        const TileVec2 point{static_cast<float>(x), static_cast<float>(y)};
        const int tileButton = static_cast<int>(button);
        const UiRect playButton{24, 52, 92, 28};
        const UiRect saveButton{124, 52, 80, 28};
        if (button == PointerButton::Primary && playButton.contains(x, y)) {
            std::string error;
            if (!tileWorldEditor_.request_play_test(&error)) { set_status(error, true, 7.0F); return; }
            (void)tileWorldEditor_.consume_play_request();
            tileWorldEditorOpen_ = false;
            (void)dispatch_action("sprite.play_level");
            return;
        }
        if (button == PointerButton::Primary && saveButton.contains(x, y)) {
            std::string error;
            if (!tileWorldEditor_.save(&error)) set_status(error, true, 7.0F);
            else set_status("Tile world and tileset saved");
            return;
        }
        const TileCanvasRect canvasViewport = tile_world_canvas_viewport();
        for (int tool = 0; tool < 7; ++tool) {
            const UiRect toolButton{static_cast<int>(canvasViewport.x) + tool * 58, 52, 54, 28};
            if (button == PointerButton::Primary && toolButton.contains(x, y)) {
                tileWorldEditor_.canvas().set_tool(static_cast<TileMapTool>(tool));
                return;
            }
        }
        if (tile_world_palette_viewport().contains(point)) {
            (void)tileWorldEditor_.palette_pointer_down(tileButton, point, tile_world_palette_viewport());
            return;
        }
        std::string error;
        if (tile_world_canvas_viewport().contains(point))
            (void)tileWorldEditor_.canvas_pointer_down(tileButton, point, tile_world_canvas_viewport(), &error);
        if (!error.empty()) set_status(error, true);
        return;
    }
    if (spriteAnimationGraphOpen_) {
        const bool control = (modifiers & 2U) != 0U;
        const bool shift = (modifiers & 1U) != 0U;
        (void)spriteAnimationGraph_.pointer_down(static_cast<int>(button),
            {static_cast<float>(x), static_cast<float>(y)}, sprite_animation_graph_viewport(), control, shift);
        return;
    }
    if (filePicker_.open) {
        if (button != PointerButton::Primary) return;
        const NativeFilePickerLayout picker = file_picker_layout();
        for (std::size_t visible = 0; visible < picker.rows.size(); ++visible) {
            if (!picker.rows[visible].contains(x, y)) continue;
            filePicker_.selected = picker.firstVisibleRow + visible;
            if (filePicker_.kind == FilePickerKind::SaveSceneAs) {
                // Clicking an existing scene fills its name in; Enter then asks before replacing.
                const auto matches = file_picker_matches();
                if (filePicker_.selected < matches.size()) {
                    // A scene outside scenes/ keeps its folder, so Enter targets that same file.
                    const std::filesystem::path& clicked = filePicker_.entries[matches[filePicker_.selected]].path;
                    std::error_code relativeError;
                    std::filesystem::path relative = std::filesystem::relative(clicked, projectRoot_, relativeError);
                    if (relativeError || relative.empty() || *relative.begin() == "..") relative = clicked;
                    relative.replace_extension();
                    filePicker_.query = relative.parent_path() == "scenes" ? relative.filename().string() : relative.generic_string();
                }
                filePicker_.confirmReplace.reset();
            } else {
                (void)accept_file_picker();
            }
            return;
        }
        if (!picker.panel.contains(x, y)) close_file_picker();
        return;
    }
    if (commandPaletteOpen_) {
        if (button != PointerButton::Primary) return;
        const NativeCommandPaletteLayout palette = command_palette_layout();
        const auto results = command_palette_results(64);
        for (std::size_t visible = 0; visible < palette.rows.size(); ++visible) {
            if (!palette.rows[visible].contains(x, y)) continue;
            const std::size_t rowIndex = palette.firstVisibleRow + visible;
            if (rowIndex < results.size()) {
                commandPaletteSelection_ = rowIndex;
                (void)activate_command_palette_selection();
            }
            return;
        }
        if (!palette.panel.contains(x, y)) {
            commandPaletteOpen_ = false;
            commandPaletteQuery_.clear();
            commandPaletteSelection_ = 0U;
        }
        return;
    }
    if (settingsPanel_.open) {
        if (button != PointerButton::Primary) return;
        const NativeSettingsModalLayout settingsLayout = settings_modal_layout();
        if (settingsLayout.applyButton.contains(x, y)) { close_settings(true); return; }
        if (settingsLayout.discardButton.contains(x, y)) { close_settings(false); return; }
        if (settingsLayout.advancedToggle.contains(x, y)) {
            settingsPanel_.includeAdvanced = !settingsPanel_.includeAdvanced;
            settingsPanel_.selectedRow = 0U;
            return;
        }
        if (settingsLayout.changedToggle.contains(x, y)) {
            settingsPanel_.changedOnly = !settingsPanel_.changedOnly;
            settingsPanel_.selectedRow = 0U;
            return;
        }
        for (std::size_t index = 0; index < settingsLayout.scopeTabs.size(); ++index) {
            if (!settingsLayout.scopeTabs[index].contains(x, y)) continue;
            if (settingsPanel_.dirty) {
                settingsPanel_.status = "Apply or discard changes before changing scope";
                return;
            }
            const SettingScope scope = index == 0U ? SettingScope::User
                : index == 1U ? SettingScope::Project : SettingScope::Session;
            settingsPanel_.open_for(scope, settingsPanel_.selectedCategory);
            return;
        }
        const auto rows = settings_rows();
        const SettingDefinition* selected = settingsPanel_.selectedRow < rows.size()
            ? rows[settingsPanel_.selectedRow] : nullptr;
        if (settingsLayout.resetSettingButton.contains(x, y)) {
            if (selected) {
                std::string error;
                if (!settingsPanel_.reset(workspace_.settings(), selected->id, &error))
                    settingsPanel_.status = error;
            }
            return;
        }
        if (settingsLayout.resetCategoryButton.contains(x, y)) {
            const std::size_t count = settingsPanel_.reset_category(
                workspace_.settings(), settingsPanel_.selectedCategory);
            settingsPanel_.status = count == 0U ? "Nothing to reset" :
                std::to_string(count) + " options will inherit defaults";
            return;
        }
        const auto categories = settings_categories();
        for (std::size_t index = 0; index < settingsLayout.categoryRows.size() && index < categories.size(); ++index) {
            if (settingsLayout.categoryRows[index].contains(x, y)) {
                settingsPanel_.selectedCategory = categories[index];
                settingsPanel_.searchQuery.clear();
                settingsPanel_.selectedRow = 0U;
                settingsPanel_.cancel_value_edit();
                return;
            }
        }
        for (std::size_t visible = 0; visible < settingsLayout.settingRows.size(); ++visible) {
            if (!settingsLayout.settingRows[visible].contains(x, y)) continue;
            const std::size_t rowIndex = settingsLayout.firstVisibleSetting + visible;
            if (rowIndex >= rows.size()) return;
            const SettingDefinition& definition = *rows[rowIndex];
            const bool alreadySelected = settingsPanel_.selectedRow == rowIndex;
            settingsPanel_.selectedRow = rowIndex;
            const bool valueColumn = x > settingsLayout.settingRows[visible].x +
                settingsLayout.settingRows[visible].width * 2 / 3;
            if (alreadySelected || valueColumn) {
                std::string error;
                if (settingsPanel_.has_choices(definition)) {
                    if (!settingsPanel_.cycle(workspace_.settings(), definition.id, 1, &error))
                        settingsPanel_.status = error;
                } else if (!settingsPanel_.begin_value_edit(workspace_.settings(), definition.id, &error)) {
                    settingsPanel_.status = error;
                }
            }
            return;
        }
        return;
    }
    if (cinematicCameraPanel_.is_open()) {
        if (button != PointerButton::Primary) return;
        const int windowWidth = layout_.menuBar.width;
        const int windowHeight = layout_.statusBar.y + layout_.statusBar.height;
        const CinematicCameraPanelLayout cameraLayout =
            cinematicCameraPanel_.layout(windowWidth, windowHeight);
        if (cameraLayout.closeButton.contains(x, y)) {
            cinematicCameraPanel_.close();
            refresh_menu_state();
            set_status("Cinematic Camera Inspector closed");
            return;
        }
        static constexpr std::array<CinematicCameraScope, 4> scopes{
            CinematicCameraScope::CameraInstance, CinematicCameraScope::ShotOverride,
            CinematicCameraScope::ProjectDefault, CinematicCameraScope::ViewportPreview};
        for (std::size_t i = 0U; i < scopes.size(); ++i) {
            if (!cameraLayout.scopeTabs[i].contains(x, y)) continue;
            cinematicCameraPanel_.set_scope(scopes[i]);
            cinematicCameraPanel_.set_status(
                std::string(EditorCinematicCameraPanel::scope_name(scopes[i])) + " camera edits");
            refresh_menu_state();
            return;
        }
        static constexpr std::array<CinematicCameraSection, 8> sections{
            CinematicCameraSection::Composition, CinematicCameraSection::Lens,
            CinematicCameraSection::FocusAndBokeh, CinematicCameraSection::SplitDiopter,
            CinematicCameraSection::ColorGrade, CinematicCameraSection::Film,
            CinematicCameraSection::Framing, CinematicCameraSection::Accessibility};
        for (std::size_t i = 0U; i < sections.size(); ++i) {
            if (!cameraLayout.sectionRows[i].contains(x, y)) continue;
            cinematicCameraPanel_.set_section(sections[i]);
            return;
        }
        static constexpr std::array<std::string_view, 10> presetActions{
            "camera.preset_neutral", "camera.preset_academy", "camera.preset_imax143",
            "camera.preset_imax190", "camera.preset_scope239", "camera.preset_anamorphic",
            "camera.preset_fisheye", "camera.preset_split_diopter",
            "camera.preset_bleach_bypass", "camera.preset_seventies"};
        for (std::size_t i = 0U; i < presetActions.size(); ++i) {
            if (cameraLayout.presetRows[i].contains(x, y)) {
                (void)dispatch_action(presetActions[i]);
                return;
            }
        }
        static constexpr std::array<std::string_view, 7> filmbackActions{
            "camera.filmback_super16", "camera.filmback_super35", "camera.filmback_full_frame",
            "camera.filmback_anamorphic35", "camera.filmback_imax15",
            "camera.filmback_imax_digital", "camera.inspector"};
        for (std::size_t i = 0U; i < filmbackActions.size(); ++i) {
            if (cameraLayout.filmbackRows[i].contains(x, y)) {
                if (i + 1U == filmbackActions.size()) {
                    cinematicCameraPanel_.set_section(CinematicCameraSection::Lens);
                    cinematicCameraPanel_.set_status("Custom filmback uses the Lens section controls");
                } else {
                    (void)dispatch_action(filmbackActions[i]);
                }
                return;
            }
        }
        if (cameraLayout.copyButton.contains(x, y)) { (void)dispatch_action("camera.copy_profile"); return; }
        if (cameraLayout.pasteButton.contains(x, y)) { (void)dispatch_action("camera.paste_profile"); return; }
        if (cameraLayout.resetButton.contains(x, y)) { (void)dispatch_action("camera.clear_effects"); return; }
        if (cameraLayout.keyframeButton.contains(x, y)) { (void)dispatch_action("camera.keyframe_profile"); return; }
        if (cameraLayout.sequencerButton.contains(x, y)) { (void)dispatch_action("camera.sequencer"); return; }
        return;
    }
    if (shortcutPanel_.open) {
        if (shortcutPanel_.capturing && button != PointerButton::NoButton) {
            ShortcutGesture gesture = mouse_shortcut(pointer_input_name(button),
                (modifiers & 2U) != 0U, (modifiers & 1U) != 0U, (modifiers & 4U) != 0U);
            std::string error;
            const SettingValue warningValue = workspace_.settings().value("input.warn_shortcut_conflicts");
            const bool overrideConflicts = std::get_if<bool>(&warningValue) && !std::get<bool>(warningValue);
            if (workspace_.shortcuts().set_binding(workspace_.shortcuts().active_profile(),
                    shortcutPanel_.captureActionId, shortcutPanel_.captureContext, shortcutPanel_.captureSlot,
                    gesture, overrideConflicts, &error)) {
                shortcutPanel_.capturing = false;
                shortcutPanel_.status = "Assigned " + format_shortcut_gesture(gesture);
                synchronize_menu_shortcuts();
            } else shortcutPanel_.status = error;
            return;
        }
        if (button != PointerButton::Primary) return;
        const NativeShortcutModalLayout shortcutLayout = shortcut_modal_layout();
        if (shortcutLayout.closeButton.contains(x, y)) { close_shortcut_editor(); return; }
        if (shortcutLayout.resetButton.contains(x, y)) {
            std::string error;
            if (workspace_.shortcuts().reset_profile(workspace_.shortcuts().active_profile(), &error)) {
                shortcutPanel_.status = "Profile reset to defaults";
                synchronize_menu_shortcuts();
            } else shortcutPanel_.status = error;
            return;
        }
        const auto cycleProfile = [&](int direction) {
            const auto profiles = workspace_.shortcuts().profile_names();
            if (profiles.empty()) return;
            const auto found = std::find(profiles.begin(), profiles.end(), std::string(workspace_.shortcuts().active_profile()));
            std::size_t index = found == profiles.end() ? 0U : static_cast<std::size_t>(std::distance(profiles.begin(), found));
            index = direction > 0 ? (index + 1U) % profiles.size() : (index + profiles.size() - 1U) % profiles.size();
            std::string error;
            if (workspace_.shortcuts().set_active_profile(profiles[index], &error)) {
                shortcutPanel_.status = "Profile: " + profiles[index];
                synchronize_menu_shortcuts();
            } else shortcutPanel_.status = error;
        };
        if (shortcutLayout.profilePrevious.contains(x, y)) { cycleProfile(-1); return; }
        if (shortcutLayout.profileNext.contains(x, y)) { cycleProfile(1); return; }
        const auto cycleContext = [&](int direction) {
            constexpr int count = static_cast<int>(ShortcutContext::ModalDialog) + 1;
            int value = shortcutPanel_.contextFilter ? static_cast<int>(*shortcutPanel_.contextFilter) : -1;
            value += direction;
            if (value < -1) value = count - 1;
            if (value >= count) value = -1;
            shortcutPanel_.contextFilter = value < 0 ? std::nullopt : std::optional<ShortcutContext>(static_cast<ShortcutContext>(value));
            shortcutPanel_.selectedRow = 0U;
        };
        if (shortcutLayout.contextPrevious.contains(x, y)) { cycleContext(-1); return; }
        if (shortcutLayout.contextNext.contains(x, y)) { cycleContext(1); return; }
        const auto rows = shortcut_rows();
        for (std::size_t visible = 0; visible < shortcutLayout.rows.size(); ++visible) {
            if (!shortcutLayout.rows[visible].contains(x, y)) continue;
            const std::size_t rowIndex = shortcutLayout.firstVisibleRow + visible;
            if (rowIndex >= rows.size()) return;
            const bool alreadySelected = shortcutPanel_.selectedRow == rowIndex;
            shortcutPanel_.selectedRow = rowIndex;
            if (alreadySelected) {
                shortcutPanel_.capturing = true;
                shortcutPanel_.captureActionId = rows[rowIndex].command->actionId;
                shortcutPanel_.captureContext = rows[rowIndex].context;
                shortcutPanel_.captureSlot = x > shortcutLayout.rows[visible].x + shortcutLayout.rows[visible].width * 5 / 6
                    ? ShortcutSlot::Secondary : ShortcutSlot::Primary;
                shortcutPanel_.status = "Press the new shortcut (Esc cancels)";
            }
            return;
        }
        return;
    }
    if (spriteAnimationGraphOpen_) {
        std::string error;
        if (!spriteAnimationGraph_.pointer_up(static_cast<int>(button),
            {static_cast<float>(x), static_cast<float>(y)}, sprite_animation_graph_viewport(), &error) &&
            !error.empty()) set_status(error, true);
        return;
    }
    if (spriteAuthoringPanel_.open()) {
        (void)spriteAuthoringPanel_.pointer_down(static_cast<int>(button), x, y, modifiers);
        return;
    }
    if (controlRigPanel_.open()) {
        (void)controlRigPanel_.pointer_down(static_cast<int>(button), x, y, modifiers);
        return;
    }
    if (button == PointerButton::Primary && chiptunePanel_.open() && chiptunePanel_.pointer_down(x, y, audioMixer_)) {
        if (const auto keys = chiptunePanel_.take_requested_key_count()) (void)set_keyboard_key_count(*keys);
        return;
    }
    if (button == PointerButton::Primary && audioEventPanel_.open() && audioEventPanel_.pointer_down(x, y, audioMixer_)) return;
    if (button == PointerButton::Primary && audioPanel_.open() && audioPanel_.pointer_down(x, y, audioMixer_)) return;
    if (button == PointerButton::Primary && synthPanel_.open() && synthPanel_.pointer_down(x, y, audioMixer_.synthesizer())) {
        if (const auto keys = synthPanel_.take_requested_key_count()) (void)set_keyboard_key_count(*keys);
        if (const auto step = synthPanel_.take_midi_port_cycle_request()) (void)cycle_midi_input_port(*step);
        return;
    }
    if (contextMenu_.open) {
        if (button == PointerButton::Primary) {
            for (std::size_t i = 0; i < contextMenu_.itemRects.size(); ++i) {
                if (contextMenu_.itemRects[i].contains(x, y)) {
                    const std::string actionId = contextMenu_.items[i].actionId;
                    close_context_menu();
                    (void)dispatch_action(actionId);
                    return;
                }
            }
        }
        close_context_menu();
        return;
    }

    if ((textEdit_.kind == TextEditKind::SlicePoint || textEdit_.kind == TextEditKind::SliceNormal) &&
        !layout_.viewport.contains(x,y)) {
        commit_text_edit();
        if (textEdit_.kind != TextEditKind::Inactive) return;
    }
    if (button == PointerButton::Primary && begin_slice_pointer(x, y, modifiers)) return;

    if (button == PointerButton::Primary && workspace_.mode() == EditorMode::Edit && !playSession_.active() &&
        !openMenu_ && !contextMenu_.open && !commandPaletteOpen_ && !filePicker_.open && !layout_.menuBar.contains(x, y)) {
        if (duplicateSpecial_.active && layout_.inspector.contains(x, y)) {
            const DuplicatePanelLayout panel = duplicate_panel_layout();
            DuplicateSpecialSettings& v = duplicateSpecial_.settings;
            const auto choose = [&](std::string message) {
                if (textEdit_.kind != TextEditKind::Inactive) commit_text_edit();
                recompute_layout();
                set_status(std::move(message) + "; " + duplicate_special_summary());
                focusRegion_ = EditorFocusRegion::Inspector;
            };
            if (panel.pivotEach.contains(x, y)) { v.pivot = DuplicatePivot::EachCopy; choose("Each copy turns about its own pivot"); return; }
            if (panel.pivotPoint.contains(x, y)) { v.pivot = DuplicatePivot::Point; choose("Copies turn around the point"); return; }
            if (panel.spaceLocal.contains(x, y)) { v.space = DuplicateSpace::Local; choose("Offsets follow each copy's turn"); return; }
            if (panel.spaceWorld.contains(x, y)) { v.space = DuplicateSpace::World; choose("Offsets along world axes"); return; }
            if (panel.pointOrigin.contains(x, y)) { v.point = {}; choose("Point: world origin"); return; }
            if (panel.pointSelection.contains(x, y)) {
                const EditorObjectBounds bounds = selection_bounds();
                v.point = bounds.valid ? multiply(add(bounds.minimum, bounds.maximum), 0.5F) : selection_pivot();
                choose("Point: selection centre");
                return;
            }
            if (panel.group.contains(x, y)) { v.group = !v.group; choose(v.group ? "Copies go in one group" : "Copies stay loose"); return; }
            if (panel.apply.contains(x, y)) { (void)apply_duplicate_special(); return; }
            if (panel.cancel.contains(x, y)) { cancel_duplicate_special("Duplicate Special cancelled"); return; }
        }
        // Numeric fields: a click on a box types into it; a press on the label starts a scrub
        // (a click on the label without dragging also types).
        for (const NumberFieldRect& rect : number_field_rects()) {
            if (rect.box.contains(x, y)) { begin_number_field_edit(rect.field, rect.axis); return; }
            if (!rect.label.contains(x, y)) continue;
            numberScrub_ = {};
            numberScrub_.armed = true;
            numberScrub_.field = rect.field;
            numberScrub_.axis = rect.axis;
            numberScrub_.startX = x;
            numberScrub_.startScalar = rect.field == NumberFieldId::BrushRadius ? static_cast<float>(voxelBrushRadius_)
                : rect.field == NumberFieldId::SurfaceOffset ? placementOffsetMeters_
                : static_cast<float>(std::strtod(number_field_text(rect.field, rect.axis).c_str(), nullptr));
            const std::string refusal = rect.field == NumberFieldId::BrushRadius || rect.field == NumberFieldId::SurfaceOffset
                ? std::string() : number_field_refusal(rect.field);
            for (const EditorObjectId id : number_field_objects(rect.field)) {
                const EditorObject* object = workspace_.document().find_object(id);
                if (!object) continue;
                if (rect.field == NumberFieldId::VoxelSize)
                    numberScrub_.sizes.push_back({id, object->transform, object->transform, object->voxelSizeMeters, object->voxelSizeMeters,
                                                  std::nullopt, std::nullopt});
                else numberScrub_.transforms.push_back({id, object->transform, object->transform});
            }
            if (!refusal.empty()) {
                numberScrub_ = {};
                set_status(refusal, true);
            }
            focusRegion_ = rect.field == NumberFieldId::BrushRadius || rect.field == NumberFieldId::SurfaceOffset
                ? EditorFocusRegion::Viewport : EditorFocusRegion::Inspector;
            return;
        }
    }

    const auto now = std::chrono::steady_clock::now();
    const SettingValue doubleMsValue = workspace_.settings().value("input.double_click_ms");
    const SettingValue doubleDistanceValue = workspace_.settings().value("input.double_click_distance_px");
    const auto doubleMs = std::get_if<std::int64_t>(&doubleMsValue)
        ? std::chrono::milliseconds(std::get<std::int64_t>(doubleMsValue)) : std::chrono::milliseconds(400);
    const int doubleDistance = std::get_if<std::int64_t>(&doubleDistanceValue)
        ? static_cast<int>(std::get<std::int64_t>(doubleDistanceValue)) : 5;
    const int clickDx = x - lastClickX_;
    const int clickDy = y - lastClickY_;
    const bool doubleClick = button == lastClickButton_ && lastClickTime_.time_since_epoch().count() != 0 &&
        now - lastClickTime_ <= doubleMs && clickDx * clickDx + clickDy * clickDy <= doubleDistance * doubleDistance;
    // Double-clicking a piece of a closed group opens that group (one level per double-click) and
    // selects the next level down.
    if (doubleClick && button == PointerButton::Primary && modifiers == 0U && layout_.viewport.contains(x, y) &&
        workspace_.mode() == EditorMode::Edit && selectionDomain_ == SelectionDomain::Objects &&
        (activeTool_ == EditorToolId::Select || activeTool_ == EditorToolId::Translate ||
         activeTool_ == EditorToolId::Rotate || activeTool_ == EditorToolId::Scale) &&
        !(activeTool_ != EditorToolId::Select && hit_test_gizmo_axis(x, y) != 0)) {
        update_hover(x, y);
        if (hoverPick_) {
            const EditorObjectId piece = hoverPick_->objectId;
            const EditorObjectId group = viewport_selection_target(piece);
            if (group != piece) {
                openGroups_.insert(group);
                workspace_.clear_selection();
                workspace_.add_to_selection(viewport_selection_target(piece));
                const EditorObject* object = workspace_.document().find_object(group);
                set_status("Opened " + (object ? object->name : std::string("group")) +
                           ": click its pieces; Edit > Close Group (Ctrl+Alt+G) closes it");
                refresh_menu_state();
                lastClickButton_ = PointerButton::NoButton;
                doubleClickConsumed_ = true;
                return;
            }
        }
    }
    if (doubleClick && !((activeTool_ == EditorToolId::Translate || activeTool_ == EditorToolId::Rotate ||
                          activeTool_ == EditorToolId::Scale) && layout_.viewport.contains(x,y) &&
                         hit_test_gizmo_axis(x,y) != 0)) {
        const ShortcutGesture gesture = mouse_shortcut(pointer_input_name(button),
            (modifiers & 2U) != 0U, (modifiers & 1U) != 0U, (modifiers & 4U) != 0U,
            ShortcutActivation::DoubleClick);
        if (dispatch_shortcut_gesture(gesture)) {
            lastClickButton_ = PointerButton::NoButton;
            doubleClickConsumed_ = true;
            return;
        }
    }

    // A click anywhere other than the field currently being edited commits it, the same way
    // losing focus commits a text field in most desktop UIs.
    if (textEdit_.kind == TextEditKind::VoxelSize || textEdit_.kind == TextEditKind::BrushRadius ||
        textEdit_.kind == TextEditKind::SurfaceOffset) {
        bool onField = false;
        for (const NumberFieldRect& rect : number_field_rects()) onField = onField || rect.box.contains(x, y);
        if (!onField) commit_text_edit();
    }
    if (textEdit_.kind == TextEditKind::HierarchyFilter && !layout_.hierarchyFilterBox.contains(x, y)) {
        commit_text_edit();
    } else if (textEdit_.kind == TextEditKind::AssetSearch && !layout_.assetSearchBox.contains(x, y)) {
        commit_text_edit();
    } else if (textEdit_.kind == TextEditKind::AssetRename) {
        commit_text_edit();
    } else if ((textEdit_.kind == TextEditKind::Position || textEdit_.kind == TextEditKind::Rotation ||
                textEdit_.kind == TextEditKind::Text3DContent || textEdit_.kind == TextEditKind::Text3DSize ||
                textEdit_.kind == TextEditKind::Text3DDepth ||
                textEdit_.kind == TextEditKind::Text3DLetterSpacing ||
                textEdit_.kind == TextEditKind::Text3DFaceMaterial ||
                textEdit_.kind == TextEditKind::Text3DSideMaterial ||
                textEdit_.kind == TextEditKind::Text3DFontPath ||
                textEdit_.kind == TextEditKind::Text3DAlignment ||
                textEdit_.kind == TextEditKind::Text3DFillRule ||
                textEdit_.kind == TextEditKind::GaborDensity ||
                textEdit_.kind == TextEditKind::GaborTint ||
                textEdit_.kind == TextEditKind::GaborEmission ||
                textEdit_.kind == TextEditKind::GaborAnisotropy ||
                textEdit_.kind == TextEditKind::GaborShadowStrength ||
                textEdit_.kind == TextEditKind::GaborLodBias) &&
               !layout_.inspector.contains(x, y)) {
        commit_text_edit();
    } else if (textEdit_.kind == TextEditKind::ObjectName) {
        commit_text_edit();
    }

    if (layout_.menuBar.contains(x, y) && button == PointerButton::Primary) {
        const auto menu = menu_name_at(x);
        if (menu) {
            if (openMenu_ && *openMenu_ == *menu) close_top_level_menu();
            else open_top_level_menu(*menu);
        }
        return;
    }
    if (openMenu_ && button == PointerButton::Primary) {
        const auto actions = menu_actions(*openMenu_);
        const NativeMenuPopupLayout popup = menu_popup_layout();
        for (std::size_t visible = 0; visible < popup.rows.size(); ++visible) {
            if (!popup.rows[visible].contains(x, y)) continue;
            const std::size_t index = popup.firstVisibleAction + visible;
            if (index < actions.size() && actions[index].enabled) {
                const std::string actionId = actions[index].id;
                close_top_level_menu();
                (void)dispatch_action(actionId);
            }
            return;
        }
        close_top_level_menu();
        return;
    }
    if (playSession_.active()) {
        if (layout_.viewport.contains(x, y) && !playCameraPossessed_) {
            const ShortcutGesture navigationGesture = mouse_shortcut(
                pointer_input_name(button),
                (modifiers & 2U) != 0U,
                (modifiers & 1U) != 0U,
                (modifiers & 4U) != 0U,
                ShortcutActivation::Hold);
            const auto navigation = workspace_.shortcuts().resolve(
                navigationGesture, active_shortcut_contexts(), settings_capabilities());
            if (navigation && navigation->actionId.starts_with("viewport.")) {
                remember_camera_position();
                activePointerCommand_ = navigation->actionId;
                dragButton_ = button;
            }
        }
        return;
    }
    if (button == PointerButton::Primary) {
        if (layout_.viewportHelpButton.contains(x, y)) {
            open_shortcut_editor();
            return;
        }
        for (std::size_t index = 0; index < layout_.toolbarButtons.size(); ++index) {
            if (layout_.toolbarButtons[index].contains(x, y)) {
                set_active_tool(kToolbarTools[index]);
                focusRegion_ = EditorFocusRegion::Toolbar;
                return;
            }
        }
        if (layout_.hierarchyFilterBox.contains(x, y)) {
            begin_text_edit(TextEditKind::HierarchyFilter, 0, hierarchyFilter_);
            focusRegion_ = EditorFocusRegion::SceneHierarchy;
            return;
        }
        const auto order = hierarchy_order();
        for (std::size_t index = 0; index < layout_.hierarchyRows.size() && index < order.size(); ++index) {
            if (layout_.hierarchyRows[index].contains(x, y)) {
                if (index < layout_.hierarchyDisclosureButtons.size() &&
                    layout_.hierarchyDisclosureButtons[index].contains(x, y)) {
                    hierarchyDrag_ = {};
                    focusRegion_ = EditorFocusRegion::SceneHierarchy;
                    (void)set_hierarchy_expanded(order[index], hierarchy_collapsed(order[index]));
                    return;
                }
                // Same rule as the viewport: Shift adds (or takes out a selected row), Ctrl removes.
                if ((modifiers & 2U) != 0U) workspace_.apply_selection({order[index]}, SelectionOperation::Subtract);
                else if ((modifiers & 1U) != 0U) workspace_.toggle_selection(order[index]);
                else workspace_.select_object(order[index]);
                focusRegion_ = EditorFocusRegion::SceneHierarchy;
                // Armed as a pending drag source; pointer_move promotes it to an active drag
                // only once the pointer has actually moved, so a plain click never reparents.
                hierarchyDrag_ = {};
                hierarchyDrag_.sourceId = order[index];
                return;
            }
        }
        for (std::size_t index = 0; index < layout_.scatterSettingRows.size(); ++index) {
            if (!layout_.scatterSettingRows[index].contains(x, y)) continue;
            focusRegion_ = EditorFocusRegion::Inspector;
            const auto field = static_cast<ScatterSettingField>(index);
            if (field == ScatterSettingField::Align) { nudge_scatter_setting(field, 1); return; }
            const ScatterSettings& v = scatterSettings_;
            char buffer[48];
            switch (field) {
                case ScatterSettingField::Count: std::snprintf(buffer, sizeof buffer, "%u", v.count); break;
                case ScatterSettingField::Seed: std::snprintf(buffer, sizeof buffer, "%llu", static_cast<unsigned long long>(v.seed)); break;
                case ScatterSettingField::Spacing: std::snprintf(buffer, sizeof buffer, "%.2f", static_cast<double>(v.minSpacingMeters)); break;
                case ScatterSettingField::YawJitter: std::snprintf(buffer, sizeof buffer, "%.0f", static_cast<double>(v.yawJitterDegrees * 0.5F)); break;
                case ScatterSettingField::MinScale: std::snprintf(buffer, sizeof buffer, "%.2f", static_cast<double>(v.minScale)); break;
                case ScatterSettingField::MaxScale: std::snprintf(buffer, sizeof buffer, "%.2f", static_cast<double>(v.maxScale)); break;
                case ScatterSettingField::BrushRadius: std::snprintf(buffer, sizeof buffer, "%.2f", static_cast<double>(v.brushRadiusMeters)); break;
                case ScatterSettingField::BrushDensity: std::snprintf(buffer, sizeof buffer, "%.0f", static_cast<double>(v.brushDensity * 100.0F)); break;
                default: buffer[0] = '\0'; break;
            }
            begin_text_edit(TextEditKind::ScatterSetting, static_cast<EditorObjectId>(index), buffer);
            return;
        }
        for (std::size_t index = 0; index < layout_.inspectorFields.size(); ++index) {
            if (!layout_.inspectorFields[index].contains(x, y)) continue;
            const auto selected = workspace_.selected_object();
            const EditorObject* object = selected ? workspace_.document().find_object(*selected) : nullptr;
            if (!object) break;
            if (index >= 2U) {
                if (object->text3d) {
                    const auto& asset = *object->text3d;
                    switch (index) {
                        case 2U: begin_text_edit(TextEditKind::Text3DContent, *selected, asset.textUtf8); break;
                        case 3U: begin_text_edit(TextEditKind::Text3DSize, *selected, std::to_string(asset.style.emSizeMeters)); break;
                        case 4U: begin_text_edit(TextEditKind::Text3DDepth, *selected, std::to_string(asset.style.extrusionDepthMeters)); break;
                        case 5U: begin_text_edit(TextEditKind::Text3DLetterSpacing, *selected, std::to_string(asset.style.letterSpacingEm)); break;
                        case 6U: begin_text_edit(TextEditKind::Text3DFaceMaterial, *selected, std::to_string(asset.style.faceMaterialId)); break;
                        case 7U: begin_text_edit(TextEditKind::Text3DSideMaterial, *selected, std::to_string(asset.style.sideMaterialId)); break;
                        case 8U: begin_text_edit(TextEditKind::Text3DFontPath, *selected, object->textFontAsset.generic_string()); break;
                        case 9U: begin_text_edit(TextEditKind::Text3DAlignment, *selected,
                            asset.style.alignment == Text3DHorizontalAlignment::Left ? "left" :
                            asset.style.alignment == Text3DHorizontalAlignment::Center ? "center" : "right"); break;
                        case 10U: begin_text_edit(TextEditKind::Text3DFillRule, *selected,
                            asset.style.fillRule == Text3DFillRule::NonZero ? "nonzero" : "evenodd"); break;
                        default: break;
                    }
                } else if (object->gaborVolume) {
                    const auto& material = object->gaborVolume->material;
                    switch (index) {
                        case 2U: begin_text_edit(TextEditKind::GaborDensity, *selected, std::to_string(material.densityMultiplier)); break;
                        case 3U: begin_text_edit(TextEditKind::GaborTint, *selected, format_float3_line(material.albedoTint)); break;
                        case 4U: begin_text_edit(TextEditKind::GaborEmission, *selected, std::to_string(material.emissionIntensity)); break;
                        case 5U: begin_text_edit(TextEditKind::GaborAnisotropy, *selected, std::to_string(material.anisotropy)); break;
                        case 6U: begin_text_edit(TextEditKind::GaborShadowStrength, *selected, std::to_string(material.shadowStrength)); break;
                        case 7U: begin_text_edit(TextEditKind::GaborLodBias, *selected, std::to_string(material.lodBias)); break;
                        default: break;
                    }
                } else break;
            } else {
                // The row's name (outside the X/Y/Z boxes): type into X; Tab moves on.
                begin_number_field_edit(index == 0 ? NumberFieldId::Position : NumberFieldId::Rotation, 0);
            }
            focusRegion_ = EditorFocusRegion::Inspector;
            return;
        }
        if (workspace_.selected_object()) {
            for (std::size_t index = 0; index < layout_.inspectorToggles.size(); ++index) {
                if (!layout_.inspectorToggles[index].contains(x, y)) continue;
                (void)toggle_inspector_flag(index);
                focusRegion_ = EditorFocusRegion::Inspector;
                return;
            }
        }
        for (std::size_t index = 0; index < layout_.bottomTabs.size(); ++index) {
            if (layout_.bottomTabs[index].contains(x, y)) {
                bottomTab_ = static_cast<BottomPanelTab>(index);
                if (bottomTab_ == BottomPanelTab::Assets && assetDatabase_.records().empty() && !assetScanInFlight_)
                    (void)start_asset_scan(false);
                recompute_layout();
                focusRegion_ = EditorFocusRegion::BottomPanel;
                return;
            }
        }
        if (bottomTab_ == BottomPanelTab::Assistant) {
            if (layout_.aiPromptBox.contains(x, y)) {
                begin_text_edit(TextEditKind::AiPrompt, 0, {});
                focusRegion_ = EditorFocusRegion::BottomPanel;
                return;
            }
            if (layout_.aiSendButton.contains(x, y)) {
                if (textEdit_.kind == TextEditKind::AiPrompt) commit_text_edit();
                else set_status("Click the AI prompt field first", true);
                return;
            }
            if (layout_.aiApproveButton.contains(x, y)) {
                const auto pending = workspace_.ai_assistant().pending_approvals();
                if (!pending.empty()) {
                    const bool ok = workspace_.ai_assistant().approve(pending.front().id);
                    set_status(ok ? "AI action approved" : "Could not approve AI action", !ok);
                    recompute_layout();
                }
                return;
            }
            if (layout_.aiDenyButton.contains(x, y)) {
                const auto pending = workspace_.ai_assistant().pending_approvals();
                if (!pending.empty()) {
                    const bool ok = workspace_.ai_assistant().deny(pending.front().id);
                    set_status(ok ? "AI action denied" : "Could not deny AI action", !ok);
                    recompute_layout();
                }
                return;
            }
        }
        if (bottomTab_ == BottomPanelTab::Assets) {
            if (layout_.assetSearchBox.contains(x, y)) {
                begin_text_edit(TextEditKind::AssetSearch, 0, assetBrowserState_.query.text);
                focusRegion_ = EditorFocusRegion::BottomPanel;
                return;
            }
            if (layout_.assetRefreshButton.contains(x, y)) {
                (void)start_asset_scan(true);
                focusRegion_ = EditorFocusRegion::BottomPanel;
                return;
            }
            if (layout_.assetFilterButton.contains(x, y)) {
                assetBrowserState_.query.staleOnly = !assetBrowserState_.query.staleOnly;
                assetBrowserState_.firstVisible = 0U;
                recompute_layout();
                set_status(assetBrowserState_.query.staleOnly ? "Showing stale and broken assets" : "Showing all assets");
                focusRegion_ = EditorFocusRegion::BottomPanel;
                return;
            }
            const auto entries = asset_browser_rows();
            for (std::size_t visible = 0; visible < layout_.assetRows.size(); ++visible) {
                const std::size_t index = assetBrowserState_.firstVisible + visible;
                if (index >= entries.size()) break;
                if (layout_.assetRows[visible].contains(x, y)) {
                    assetBrowserState_.selectedId = entries[index]->id;
                    refresh_menu_state();
                    if (button == PointerButton::Primary && !doubleClick) begin_asset_drag(*entries[index], x, y);
                    if (doubleClick && (entries[index]->relativePath.extension() == ".dverig" ||
                                        entries[index]->relativePath.extension() == ".dverigui")) {
                        (void)dispatch_action("asset.open");
                        doubleClickConsumed_ = true;
                        return;
                    }
                    set_status("Selected asset: " + entries[index]->relativePath.generic_string());
                    focusRegion_ = EditorFocusRegion::BottomPanel;
                    return;
                }
            }
        }
    }
    if (!layout_.viewport.contains(x, y)) {
        if (button == PointerButton::Extra1)
            (void)dispatch_shortcut_gesture(mouse_shortcut("mouse4", (modifiers & 2U) != 0U,
                                                             (modifiers & 1U) != 0U, (modifiers & 4U) != 0U));
        return;
    }
    focusRegion_ = EditorFocusRegion::Viewport;
    // Loop-cut slide only captures clicks that reached the viewport itself: modal panels,
    // palettes, toolbars and overlays above it have already consumed theirs.
    if (polygonLoopCutPreview_ && button==PointerButton::Primary && !(modifiers&4U)) {
        polygonLoopCutDragging_=true; polygonLoopCutStartX_=x;
        polygonLoopCutStartFraction_=polygonPreviewSettings_.amount; return;
    }
    if (button == PointerButton::Extra1) {
        const SettingValue overrideValue = workspace_.settings().value("input.mouse4_viewport");
        const std::string overrideAction = std::get_if<std::string>(&overrideValue)
            ? std::get<std::string>(overrideValue) : std::string("profile");
        if (overrideAction == "none") return;
        if (overrideAction == "previous_view") { (void)dispatch_action("view.previous_camera"); return; }
        if (overrideAction == "sample_material") { (void)dispatch_action("voxel.sample_material"); return; }
        (void)dispatch_shortcut_gesture(mouse_shortcut("mouse4", (modifiers & 2U) != 0U,
                                                         (modifiers & 1U) != 0U, (modifiers & 4U) != 0U));
        return;
    }
    const ShortcutGesture navigationGesture = mouse_shortcut(pointer_input_name(button),
        (modifiers & 2U) != 0U, (modifiers & 1U) != 0U, (modifiers & 4U) != 0U, ShortcutActivation::Hold);
    const auto navigation = workspace_.shortcuts().resolve(navigationGesture, active_shortcut_contexts(),
                                                           settings_capabilities());
    if (navigation && navigation->actionId.starts_with("viewport.")) {
        remember_camera_position();
        activePointerCommand_ = navigation->actionId;
        dragButton_ = button;
        orbitPress_.reset();
        if (navigation->actionId == "viewport.orbit" && button == PointerButton::Primary && (modifiers & 4U) != 0U)
            orbitPress_ = std::pair{x, y};
        return;
    }
    dragButton_ = button;
    if (button == PointerButton::Primary) {
        if (polygonPreviewBefore_) return;
        if (floodFillArmed_) {
            update_hover(x, y);
            if (hoverPick_) (void)flood_fill_at(hoverPick_->objectId, hoverPick_->voxel);
            else set_status("Flood Fill: click a voxel (Esc cancels)", true);
            return;
        }
        if (placementMode_ != PlacementTarget::NoTarget) {
            update_placement_preview(x, y);
            if (placementPoint_) finish_placement(false);
            return;
        }
        update_hover(x, y);
        const bool selectLikeTool = activeTool_ == EditorToolId::Select || activeTool_ == EditorToolId::Translate ||
                                    activeTool_ == EditorToolId::Rotate || activeTool_ == EditorToolId::Scale;
        if (activeTool_ == EditorToolId::Select && begin_shape_selection(x, y, modifiers)) return;
        if (selectionDomain_ == SelectionDomain::Polygons && selectLikeTool) {
            prune_polygon_selection();
            if (!polygonSelection_.components.empty() && activeTool_ != EditorToolId::Select && hit_test_gizmo_axis(x,y)) {
                begin_gizmo_drag(x,y,modifiers); return;
            }
            marquee_.active = true;
            marquee_.operation = selection_operation_for_modifiers(modifiers);
            marquee_.startX = marquee_.currentX = x; marquee_.startY = marquee_.currentY = y;
            return;
        }
        if (selectionDomain_ == SelectionDomain::Voxels && selectLikeTool) {
            prune_voxel_selection();
            if (activeTool_ == EditorToolId::Translate && !voxelSelection_.empty()) {
                if (const int axis = hit_test_gizmo_axis(x, y); axis != 0) {
                    close_previews_for_voxel_edit("voxel move started");
                    voxelMoveAxis_ = axis;
                    voxelMoveStartX_ = x;
                    voxelMoveStartY_ = y;
                    voxelMoveSteps_ = 0;
                    return;
                }
            }
            // Box modifiers match polygon selection. Brush selection has its own explicit mode
            // so Ctrl+Shift can intersect a box; brush dabs add, or remove while Ctrl is held.
            if (voxelSelectionBrushEnabled_ && activeTool_ == EditorToolId::Select) {
                voxelBrushSelecting_ = true;
                voxelBrushOperation_ = (modifiers & 2U) ? SelectionOperation::Subtract : SelectionOperation::Add;
                voxel_brush_select_at(x, y);
                return;
            }
            const SelectionOperation operation = selection_operation_for_modifiers(modifiers);
            marquee_.active = true;
            marquee_.clickObject.reset();
            marquee_.clickVoxel = hoverPick_;
            marquee_.operation = operation;
            marquee_.startX = marquee_.currentX = x;
            marquee_.startY = marquee_.currentY = y;
            return;
        }
        if ((activeTool_ == EditorToolId::Translate || activeTool_ == EditorToolId::Rotate ||
             activeTool_ == EditorToolId::Scale) &&
            hit_test_gizmo_axis(x, y) != 0) {
            begin_gizmo_drag(x, y, modifiers);
        } else if (activeTool_ == EditorToolId::Select || activeTool_ == EditorToolId::Translate ||
                   activeTool_ == EditorToolId::Rotate || activeTool_ == EditorToolId::Scale) {
            // Defer selection until release so a box can start over geometry. Shift adds,
            // Ctrl removes, Shift+Ctrl intersects the current set (selection_operation_for_modifiers).
            marquee_.active = true;
            marquee_.clickObject = hoverPick_ ? std::optional<EditorObjectId>(hoverPick_->objectId) : std::nullopt;
            marquee_.operation = selection_operation_for_modifiers(modifiers);
            marquee_.startX = marquee_.currentX = x;
            marquee_.startY = marquee_.currentY = y;
        } else if (activeTool_ == EditorToolId::ScatterBrush) {
            if ((modifiers & 2U) != 0U) {
                brushResizing_ = true;
                brushResizeLastX_ = x;
            } else if (brushSourceRoots_.empty() && !brushPrefab_ && (modifiers & 1U) == 0U) {
                set_status("Nothing to paint: select objects (or a prefab in Assets), then pick the Scatter tool", true, 6.0F);
            } else {
                brushStroking_ = true;
                brushErasing_ = (modifiers & 1U) != 0U;
                brushPending_.clear();
                brushEraseIds_.clear();
                brushLastDab_.reset();
                update_brush_cursor(x, y);
                brush_dab();
            }
        } else {
            begin_voxel_stroke(x, y);
        }
    }
    (void)modifiers;
}

void NativeEditorController::pointer_up(PointerButton button, int x, int y, std::uint32_t modifiers) {
    if (numberScrub_.armed && button == PointerButton::Primary) {
        update_number_scrub(x, modifiers);
        finish_number_scrub(false);
        lastPointerX_ = x; lastPointerY_ = y;
        return;
    }
    if (polygonLoopCutDragging_ && button==PointerButton::Primary) {
        pointer_move(x,y,modifiers); polygonLoopCutDragging_=false; return;
    }
    if (assetDrag_.armed && button == PointerButton::Primary) {
        finish_asset_drag(x, y);
        dragButton_ = PointerButton::NoButton;
        lastPointerX_ = x; lastPointerY_ = y;
        return;
    }
    if(mesh_authoring_pointer_up(button,x,y,modifiers)){dragButton_=PointerButton::NoButton;refresh_menu_state();return;}
    if (button == PointerButton::Primary && sliceDrag_) {
        update_slice_pointer(x, y, modifiers);
        finish_slice_pointer(false);
        lastPointerX_ = x; lastPointerY_ = y;
        return;
    }
    if (longJob_ || importOptions_.open) {
        // Nothing that started before the job may finish into the scene now.
        dragButton_ = PointerButton::NoButton;
        activePointerCommand_.clear();
        return;
    }
    if (playSession_.active()) {
        const std::string input = pointer_input_name(button);
        if (!input.empty()) playSession_.set_action_pressed(input, false);
        if (button == PointerButton::Primary) playSession_.set_action_pressed("fire", false);
        if (button == PointerButton::Secondary) playSession_.set_action_pressed("aim", false);
    }
    if (spritePixelArtOpen_) {
        if (spritePixelPainting_) {
            const UiRect canvas = sprite_pixel_art_canvas();
            const auto& document = spritePixelArt_.document();
            const int scale = std::max(1, std::min(canvas.width / static_cast<int>(document.width),
                                                   canvas.height / static_cast<int>(document.height)));
            const int originX = canvas.x + (canvas.width - static_cast<int>(document.width) * scale) / 2;
            const int originY = canvas.y + (canvas.height - static_cast<int>(document.height) * scale) / 2;
            const int px = (x - originX) / scale;
            const int py = static_cast<int>(document.height) - 1 - (y - originY) / scale;
            if (px != spritePixelLastX_ || py != spritePixelLastY_) {
                std::string error;
                if (!spritePixelArt_.draw_line(spritePixelLastX_, spritePixelLastY_, px, py, &error) && !error.empty())
                    set_status(error, true);
                spritePixelLastX_ = px; spritePixelLastY_ = py;
            }
        }
        lastPointerX_ = x; lastPointerY_ = y; return;
    }
    if (spriteRig2DOpen_) { lastPointerX_ = x; lastPointerY_ = y; return; }
    if (spritePixelArtOpen_) { spritePixelPainting_ = false; return; }
    if (spriteRig2DOpen_) return;
    if (tileWorldEditorOpen_) {
        const TileVec2 point{static_cast<float>(x), static_cast<float>(y)};
        std::string error;
        (void)tileWorldEditor_.palette_pointer_up(static_cast<int>(button));
        (void)tileWorldEditor_.canvas_pointer_up(static_cast<int>(button), point,
                                                 tile_world_canvas_viewport(), &error);
        if (!error.empty()) set_status(error, true);
        return;
    }
    if (spriteAuthoringPanel_.open()) {
        (void)spriteAuthoringPanel_.pointer_up(static_cast<int>(button), x, y, modifiers);
        return;
    }
    if (controlRigPanel_.open()) {
        (void)controlRigPanel_.pointer_up(static_cast<int>(button), x, y, modifiers);
        return;
    }
    if (button == PointerButton::Primary && chiptunePanel_.open() && chiptunePanel_.pointer_up(x, y)) return;
    if (button == PointerButton::Primary && audioEventPanel_.open() && audioEventPanel_.pointer_up(x, y)) return;
    if (button == PointerButton::Primary && synthPanel_.open() && synthPanel_.pointer_up(x, y, audioMixer_.synthesizer())) return;
    if (pending_destructive_confirmation()) return;

    const int completedClickDistance = (x - pointerDownX_) * (x - pointerDownX_) +
                                       (y - pointerDownY_) * (y - pointerDownY_);
    // A closed lasso ends where it started but is a drag, not a click.
    if (!doubleClickConsumed_ && completedClickDistance <= 25 && lassoPath_.size() < 3U) {
        lastClickButton_ = button;
        lastClickX_ = x;
        lastClickY_ = y;
        lastClickTime_ = std::chrono::steady_clock::now();
    }
    doubleClickConsumed_ = false;

    if (button == PointerButton::Secondary) {
        const int distanceSquared = (x - pointerDownX_) * (x - pointerDownX_) + (y - pointerDownY_) * (y - pointerDownY_);
        if (distanceSquared <= 25) {
            // A right-click that didn't drag (didn't just orbit the camera): open a context
            // menu for whatever is under the cursor, in the viewport or the hierarchy.
            std::optional<NumberFieldRect> fieldHit;
            if (workspace_.mode() == EditorMode::Edit)
                for (const NumberFieldRect& rect : number_field_rects())
                    if (rect.label.contains(x, y) || rect.box.contains(x, y)) { fieldHit = rect; break; }
            if (fieldHit) {
                // Numeric field: reset, copy or paste this value.
                contextNumberField_ = std::pair{fieldHit->field, fieldHit->axis};
                const bool perAxis = fieldHit->field == NumberFieldId::Position || fieldHit->field == NumberFieldId::Rotation;
                contextMenu_ = {};
                contextMenu_.open = true;
                const std::string what = perAxis ? std::string(fieldHit->axis == 0 ? "X" : fieldHit->axis == 1 ? "Y" : "Z")
                                                 : std::string(fieldHit->field == NumberFieldId::VoxelSize ? "Voxel size"
                                                               : fieldHit->field == NumberFieldId::BrushRadius ? "Brush radius" : "Surface offset");
                contextMenu_.items.push_back({"inspector.field.reset", "Reset " + what +
                    (fieldHit->field == NumberFieldId::VoxelSize ? " (0.1 m)" : " (0)")});
                if (perAxis)
                    contextMenu_.items.push_back({"inspector.field.reset_row",
                        std::string("Reset ") + (fieldHit->field == NumberFieldId::Position ? "Position" : "Rotation") + " X, Y, Z"});
                contextMenu_.items.push_back({"inspector.field.copy", "Copy " + what});
                if (!numberFieldClipboard_.empty())
                    contextMenu_.items.push_back({"inspector.field.paste", "Paste " + numberFieldClipboard_});
                const int itemHeight = static_cast<int>(24.0F * kLogicalLayoutScale);
                constexpr int popupWidth = 190;
                const int popupHeight = itemHeight * static_cast<int>(contextMenu_.items.size());
                contextMenu_.x = std::clamp(x, 0, std::max(0, width_ - popupWidth));
                contextMenu_.y = std::clamp(y, layout_.menuBar.height, std::max(layout_.menuBar.height, height_ - popupHeight));
                for (std::size_t i = 0; i < contextMenu_.items.size(); ++i)
                    contextMenu_.itemRects.push_back({contextMenu_.x, contextMenu_.y + static_cast<int>(i) * itemHeight, popupWidth, itemHeight});
            } else if (layout_.viewport.contains(x, y)) {
                update_hover(x, y);
                open_context_menu(x, y, hoverPick_ ? std::optional(hoverPick_->objectId) : std::nullopt, false);
            } else if (layout_.hierarchy.contains(x, y)) {
                open_context_menu(x, y, hierarchy_row_object_at(x, y), true);
            }
        }
    }

    if (hierarchyDrag_.sourceId != 0) {
        if (hierarchyDrag_.active) {
            const EditorObject* source = workspace_.document().find_object(hierarchyDrag_.sourceId);
            if (source) {
                const std::optional<EditorObjectId> newParent =
                    hierarchyDrag_.hoverTarget && *hierarchyDrag_.hoverTarget != hierarchyDrag_.sourceId
                        ? hierarchyDrag_.hoverTarget
                        : std::nullopt;
                if (newParent != source->parent) {
                    ObjectReparentChange change{hierarchyDrag_.sourceId, source->parent, newParent};
                    const CommandResult result = workspace_.commands().execute(
                        workspace_.document(),
                        std::make_unique<ReparentObjectsCommand>(std::vector{change}, "Reparent object"));
                    if (result.success) recompute_layout();
                    set_status(result.success ? "Reparented object" : result.message, !result.success);
                }
            }
        }
        hierarchyDrag_ = {};
    }

    if (button == PointerButton::Primary) {
        if (voxelBrushSelecting_) voxelBrushSelecting_ = false;
        if (paintSelecting_) { paintSelecting_ = false; lastPaintDab_.reset(); }
        if (marquee_.active && !lassoPath_.empty() && finish_lasso_selection(x, y)) marquee_ = {};
        lassoPath_.clear();
        if (voxelMoveAxis_ != 0) {
            const auto offset = voxel_move_preview();
            voxelMoveAxis_ = 0;
            voxelMoveSteps_ = 0;
            if (offset && (offset->x != 0 || offset->y != 0 || offset->z != 0)) (void)move_selected_voxels(*offset);
        }
        if (marquee_.active && selectionDomain_ == SelectionDomain::Polygons) finish_polygon_marquee(x,y);
        if (marquee_.active && selectionDomain_ == SelectionDomain::Voxels) finish_voxel_marquee(x, y);
        if (marquee_.active) {
            const int distanceSquared = (x - marquee_.startX) * (x - marquee_.startX) + (y - marquee_.startY) * (y - marquee_.startY);
            std::set<EditorObjectId> hits;
            if (distanceSquared <= 25) {
                // Pieces of a closed group answer for the group; masked-out objects are skipped so
                // the click reaches whatever is behind them.
                const auto clickTargets = marquee_.clickObject || (selectionMask_.helpers && selectionDomain_ == SelectionDomain::Objects) ? selectable_targets_at(marquee_.startX, marquee_.startY)
                                                               : std::vector<EditorPickResult>{};
                if (!clickTargets.empty()) {
                    if ((modifiers & 7U) == 0U || (modifiers & 4U) != 0U) {
                        const auto& depthHits = clickTargets;
                        if ((modifiers & 4U) != 0U && (modifiers & 3U) == 0U && depthHits.size() > 1U) {
                            open_pick_list(x, y, depthHits);
                            marquee_ = {};
                            return;
                        }
                        if ((modifiers & 7U) == 0U && depthHits.size() > 1U) {
                            const auto now = std::chrono::steady_clock::now();
                            const int dx = x - lastPickX_, dy = y - lastPickY_;
                            if (dx * dx + dy * dy <= 25 &&
                                now - lastPickTime_ < std::chrono::milliseconds(750))
                                pickCycleIndex_ = (pickCycleIndex_ + 1U) % depthHits.size();
                            else pickCycleIndex_ = 0U;
                            lastPickTime_ = now;
                            lastPickX_ = x; lastPickY_ = y;
                            hits.insert(depthHits[pickCycleIndex_].objectId);
                        } else hits.insert(depthHits.front().objectId);
                    } else hits.insert(clickTargets.front().objectId);
                }
            } else {
                const int left = std::min(marquee_.startX, x);
                const int right = std::max(marquee_.startX, x);
                const int top = std::min(marquee_.startY, y);
                const int bottom = std::max(marquee_.startY, y);
                const bool contain = x >= marquee_.startX;
                const UiRect rectangle{left, top, right - left + 1, bottom - top + 1};
                // Pieces inside a closed group count for the group: a crossing box takes the group if
                // any piece is touched, a window box only if every piece with geometry is inside.
                std::map<EditorObjectId, std::pair<std::size_t, std::size_t>> groupPieces;  // matched, total
                for (const auto& [id, object] : workspace_.document().objects()) {
                    // Box selection only takes what is on screen: isolated-out objects are skipped
                    // (object_matches_screen_rect already skips hidden ones).
                    if (is_isolated_out(id)) continue;
                    const EditorObjectId target = viewport_selection_target(id);
                    const bool matched = object_matches_screen_rect(object, camera_, layout_.viewport, rectangle, contain);
                    if (target == id) { if (matched) hits.insert(id); continue; }
                    if (!has_own_geometry(object) || !object.flags.visible) continue;
                    auto& [matchedCount, total] = groupPieces[target];
                    ++total;
                    if (matched) ++matchedCount;
                }
                for (const auto& [group, counts] : groupPieces)
                    if (contain ? counts.first == counts.second && counts.second > 0 : counts.first > 0) hits.insert(group);
                std::erase_if(hits, [&](EditorObjectId id) { return !object_selectable(id); });
                // Front-only unless X-Ray Selection: keep what actually shows inside the box.
                if (!viewportSettings_.xraySelection && !hits.empty()) hits = targets_seen_in_rect(rectangle, hits);
                // Helpers are drawn as markers over the scene, so a box takes any marker inside it.
                if (selectionMask_.helpers)
                    for (const HelperMarker& marker : helper_markers())
                        if (rectangle.contains(static_cast<int>(marker.x), static_cast<int>(marker.y)) && object_selectable(marker.id))
                            hits.insert(marker.id);
            }
            SelectionOperation operation = marquee_.operation;
            // Shift+click on something already selected takes it out again.
            if (distanceSquared <= 25 && operation == SelectionOperation::Add && !hits.empty() &&
                std::all_of(hits.begin(), hits.end(), [&](EditorObjectId id) { return workspace_.is_selected(id); }))
                operation = SelectionOperation::Subtract;
            workspace_.apply_selection(hits, operation);
            marquee_ = {};
        }
        if (gizmoDragging_) finish_gizmo_drag(false);
        finish_voxel_stroke();
        brushResizing_ = false;
        if (brushStroking_) finish_brush_stroke(false);
    }
    if (dragButton_ == button && orbitPress_ && activePointerCommand_ == "viewport.orbit") {
        // Alt+click without dragging (an orbit that never moved) opens the depth-sorted pick list
        // of everything under the cursor, so it is reachable while Alt+drag orbits.
        const int dx = x - orbitPress_->first, dy = y - orbitPress_->second;
        const bool stationary = dx * dx + dy * dy <= 25;
        orbitPress_.reset();
        if (stationary && workspace_.mode() == EditorMode::Edit && !playSession_.active() &&
            selectionDomain_ == SelectionDomain::Objects && layout_.viewport.contains(x, y)) {
            const auto targets = selectable_targets_at(x, y);
            if (targets.size() > 1U) open_pick_list(x, y, targets);
        }
    }
    if (dragButton_ == button) {
        dragButton_ = PointerButton::NoButton;
        activePointerCommand_.clear();
        reset_camera_navigation();
    }
    update_hover(x, y);
    (void)modifiers;
}

void NativeEditorController::pointer_wheel(float steps, int x, int y, std::uint32_t modifiers) {
    if(!settingsPanel_.open && !longJob_ && mesh_authoring_wheel(steps,x,y))return;
    if (sliceDrag_) return;
    hoverX_ = x;
    hoverY_ = y;
    if (longJob_) return;
    if (importOptions_.open) {
        const NativeImportOptionsLayout panel = import_options_layout();
        for (std::size_t i = 0; i < panel.rows.size(); ++i)
            if (panel.rows[i].contains(x, y) && panel.rowIndex[i] < 3) {
                importOptions_.selected = panel.rowIndex[i];
                nudge_import_option(panel.rowIndex[i], steps > 0.0F ? 1 : -1);
            }
        return;
    }
    if (commandPaletteOpen_) {
        const auto results = command_palette_results(64);
        if (!results.empty() && steps != 0.0F) {
            const int delta = steps > 0.0F ? -3 : 3;
            const std::int64_t next = std::clamp<std::int64_t>(
                static_cast<std::int64_t>(commandPaletteSelection_) + delta,
                0, static_cast<std::int64_t>(results.size() - 1U));
            commandPaletteSelection_ = static_cast<std::size_t>(next);
        }
        return;
    }
    if (settingsPanel_.open) {
        const auto rows = settings_rows();
        if (!rows.empty()) {
            const int delta = steps > 0.0F ? -1 : 1;
            const std::int64_t next = std::clamp<std::int64_t>(
                static_cast<std::int64_t>(settingsPanel_.selectedRow) + delta,
                0, static_cast<std::int64_t>(rows.size() - 1U));
            settingsPanel_.selectedRow = static_cast<std::size_t>(next);
        }
        return;
    }
    if (tileWorldEditorOpen_) {
        const TileVec2 point{static_cast<float>(x), static_cast<float>(y)};
        if (tile_world_palette_viewport().contains(point))
            (void)tileWorldEditor_.palette_wheel(steps, point, tile_world_palette_viewport());
        else if (tile_world_canvas_viewport().contains(point))
            (void)tileWorldEditor_.canvas_wheel(steps, point, tile_world_canvas_viewport());
        return;
    }
    if (spriteAnimationGraphOpen_) {
        (void)spriteAnimationGraph_.wheel(steps, {static_cast<float>(x), static_cast<float>(y)},
                                          sprite_animation_graph_viewport());
        return;
    }
    if (spriteAuthoringPanel_.open()) {
        (void)spriteAuthoringPanel_.pointer_wheel(steps, x, y, modifiers);
        return;
    }
    if (controlRigPanel_.open()) {
        (void)controlRigPanel_.pointer_wheel(steps, x, y, modifiers);
        return;
    }
    if (chiptunePanel_.open() && chiptunePanel_.pointer_wheel(steps, x, y)) return;
    if (synthPanel_.open() && synthPanel_.pointer_wheel(steps, x, y)) return;
    if (shortcutPanel_.open) {
        if (shortcutPanel_.capturing) {
            ShortcutGesture gesture = wheel_shortcut(steps > 0.0F, (modifiers & 2U) != 0U,
                                                      (modifiers & 1U) != 0U, (modifiers & 4U) != 0U);
            std::string error;
            const SettingValue warningValue = workspace_.settings().value("input.warn_shortcut_conflicts");
            const bool overrideConflicts = std::get_if<bool>(&warningValue) && !std::get<bool>(warningValue);
            if (workspace_.shortcuts().set_binding(workspace_.shortcuts().active_profile(),
                    shortcutPanel_.captureActionId, shortcutPanel_.captureContext, shortcutPanel_.captureSlot,
                    gesture, overrideConflicts, &error)) {
                shortcutPanel_.capturing = false;
                shortcutPanel_.status = "Assigned " + format_shortcut_gesture(gesture);
                synchronize_menu_shortcuts();
            } else shortcutPanel_.status = error;
            return;
        }
        const auto rows = shortcut_rows();
        if (!rows.empty()) {
            const int delta = steps > 0.0F ? -1 : 1;
            const std::int64_t next = std::clamp<std::int64_t>(
                static_cast<std::int64_t>(shortcutPanel_.selectedRow) + delta,
                0, static_cast<std::int64_t>(rows.size() - 1U));
            shortcutPanel_.selectedRow = static_cast<std::size_t>(next);
        }
        return;
    }
    if (!openMenu_ && layout_.hierarchyScrollMax > 0 && layout_.hierarchy.contains(x, y) && steps != 0.0F) {
        hierarchyScroll_ = std::clamp(hierarchyScroll_ - static_cast<int>(std::lround(steps * 3.0F)), 0,
                                      layout_.hierarchyScrollMax);
        recompute_layout();
        return;
    }
    if (!openMenu_ && layout_.inspectorScrollMax > 0 && layout_.inspector.contains(x, y) && steps != 0.0F) {
        inspectorScroll_ = std::clamp(inspectorScroll_ - static_cast<int>(std::lround(steps * 22.0F)), 0,
                                      layout_.inspectorScrollMax);
        recompute_layout();
        return;
    }
    if (!openMenu_ && steps != 0.0F) {
        for (std::size_t index = 0; index < layout_.scatterSettingRows.size(); ++index) {
            if (!layout_.scatterSettingRows[index].contains(x, y)) continue;
            nudge_scatter_setting(static_cast<ScatterSettingField>(index), steps > 0.0F ? 1 : -1);
            return;
        }
    }
    if (!openMenu_ && !playSession_.active() && activeTool_ == EditorToolId::ScatterBrush && (modifiers & 2U) != 0U &&
        layout_.viewport.contains(x, y) && steps != 0.0F) {
        set_scatter_brush_radius(scatterSettings_.brushRadiusMeters * std::pow(1.15F, steps));
        return;
    }
    if (openMenu_) {
        const auto actions = menu_actions(*openMenu_);
        const NativeMenuPopupLayout popup = menu_popup_layout();
        if (popup.popup.contains(x, y) && actions.size() > popup.rows.size()) {
            const std::size_t maximumOffset = actions.size() - popup.rows.size();
            if (steps > 0.0F) {
                if (menuScrollOffset_ > 0U) --menuScrollOffset_;
            } else if (steps < 0.0F) {
                menuScrollOffset_ = std::min(menuScrollOffset_ + 1U, maximumOffset);
            }
            menuHoveredAction_.reset();
        }
        return;
    }
    if (bottomTab_ == BottomPanelTab::Assets && layout_.bottomPanel.contains(x, y)) {
        const auto rows = asset_browser_rows();
        const std::size_t visible = std::max<std::size_t>(1U, layout_.assetRows.size());
        const std::size_t maximum = rows.size() > visible ? rows.size() - visible : 0U;
        if (steps > 0.0F) {
            const std::size_t amount = std::min<std::size_t>(3U, assetBrowserState_.firstVisible);
            assetBrowserState_.firstVisible -= amount;
        } else if (steps < 0.0F) {
            assetBrowserState_.firstVisible = std::min(assetBrowserState_.firstVisible + 3U, maximum);
        }
        recompute_layout();
        return;
    }
    if (!layout_.viewport.contains(x, y)) return;
    const ShortcutGesture gesture = wheel_shortcut(steps > 0.0F, (modifiers & 2U) != 0U,
                                                    (modifiers & 1U) != 0U, (modifiers & 4U) != 0U);
    if (dispatch_shortcut_gesture(gesture)) return;
}

const EditorToolInfo& editor_tool_info(EditorToolId tool) noexcept {
    static constexpr std::array<EditorToolInfo, static_cast<std::size_t>(EditorToolId::ScatterBrush) + 1U> kTools{{
        {"Select", "Sel", "transform.select", "Click an object to select it (a piece of a group selects the group; double-click opens it); drag empty space to box-select.",
         "Objects"},
        {"Move", "Mov", "transform.translate", "Drag a gizmo axis to move the selection.", "Selected objects"},
        {"Add", "Add", "voxel.brush", "Click a voxel face to add voxels of the active material; [ / ] radius, Voxel > Brush Shape cube or sphere; green cells show what a click adds.",
         "Voxel objects"},
        {"Remove", "Rem", "voxel.erase", "Click voxels to remove them; [ / ] radius, Voxel > Brush Shape; red cells show what a click removes.", "Voxel objects"},
        {"Paint", "Pnt", "voxel.paint_material", "Click voxels to repaint them with the active material; [ / ] radius, Voxel > Brush Shape; outlined cells show what a click repaints.",
         "Voxel objects"},
        {"Box", "Box", "voxel.volume", "Click a voxel to add a 3x3x3 box around it.", "Voxel objects"},
        {"Beam", "Bm", "voxel.line", "Click a voxel to add a 5-voxel beam along +X.", "Voxel objects"},
        {"Anchor", "Anc", "", "Click a voxel to toggle it as a structural anchor.", "Voxel objects"},
        {"Rotate", "Rot", "transform.rotate", "Drag a gizmo ring to rotate the selection.", "Selected objects"},
        {"Scale", "Scl", "transform.scale", "Drag to scale a local axis without resampling; Alt: uniform; Ctrl at start: resample.",
         "Voxel objects"},
        {"Scatter", "Sct", "scatter.brush", "Drag to paint copies of the selected objects; Shift-drag erases copies; Ctrl+wheel sets the radius.",
         "Any visible voxel or polygon surface"},
    }};
    const auto index = static_cast<std::size_t>(tool);
    return kTools[index < kTools.size() ? index : 0U];
}

std::string NativeEditorController::tool_shortcut_text(EditorToolId tool) const {
    const EditorToolInfo& info = editor_tool_info(tool);
    if (info.actionId.empty()) return {};
    static constexpr std::array<ShortcutContext, 2> kContexts{ShortcutContext::Viewport, ShortcutContext::VoxelEditor};
    return workspace_.shortcuts().display_binding(info.actionId, kContexts);
}

std::vector<std::string> NativeEditorController::viewport_tool_gestures() const {
    const auto& shortcuts = workspace_.shortcuts();
    static constexpr std::array<ShortcutContext, 3> kContexts{
        ShortcutContext::VoxelEditor, ShortcutContext::Viewport, ShortcutContext::Camera};
    const auto binding = [&](std::string_view actionId) { return shortcuts.display_binding(actionId, kContexts); };
    const auto pair = [&](std::string_view down, std::string_view up, std::string_view what) {
        const std::string a = binding(down);
        const std::string b = binding(up);
        if (a.empty() || b.empty()) return std::string{};
        return a + "/" + b + " " + std::string(what);
    };
    std::vector<std::string> gestures;
    const auto add = [&](std::string text) { if (!text.empty()) gestures.push_back(std::move(text)); };
    const auto keyed = [&](std::string_view actionId, std::string_view what) {
        const std::string key = binding(actionId);
        return key.empty() ? std::string{} : key + " " + std::string(what);
    };
    if (camera_mode() == camera::CameraRigMode::FreeFly) {
        add("RMB free look");
        add("WASDQE fly");
        add("Shift boost");
        return gestures;
    }
    if (selectionDomain_==SelectionDomain::Polygons) {
        add("Polygon: click / box; Shift add, Ctrl remove");
        add("1 vertices  2 edges  3 faces  4 objects");
        if (activeTool_==EditorToolId::Translate||activeTool_==EditorToolId::Rotate||activeTool_==EditorToolId::Scale)
            add("Drag axis/ring; type value; Enter apply, Esc cancel");
        return gestures;
    }
    if (selectionDomain_ == SelectionDomain::Voxels &&
        (activeTool_ == EditorToolId::Select || activeTool_ == EditorToolId::Translate ||
         activeTool_ == EditorToolId::Rotate || activeTool_ == EditorToolId::Scale)) {
        if (voxelSelectionBrushEnabled_ && activeTool_ == EditorToolId::Select) {
            add("Selection Brush: drag adds, Ctrl removes");
            add("[ / ] brush radius");
            add("Voxel > Selection Brush: return to boxes");
            return gestures;
        }
        if (activeTool_ == EditorToolId::Rotate || activeTool_ == EditorToolId::Scale)
            add("Voxel menu: region rotation / integer scale");
        if (activeTool_ == EditorToolId::Translate && !voxelSelection_.empty())
            add("Drag axis: move voxels");
        add(voxelClickSelection_ == VoxelClickSelection::Cell ? "Voxels: click one cell" : "Voxels: click piece");
        add("Box: rightward contains, leftward crosses");
        add("Shift add  Ctrl remove  Ctrl+Shift intersect");
        return gestures;
    }
    const bool transformTool = activeTool_ == EditorToolId::Translate || activeTool_ == EditorToolId::Rotate ||
                               activeTool_ == EditorToolId::Scale;
    if (transformTool && workspace_.selected_objects().empty()) {
        // Say what the tool needs instead of listing gestures that cannot work yet.
        const std::string selectKey = binding("transform.select");
        add("Select an object first" + (selectKey.empty() ? std::string() : " (" + selectKey + " select tool)"));
        return gestures;
    }
    switch (activeTool_) {
        case EditorToolId::Select:
            add("Click select");
            add("Shift-click add");
            add("Drag empty space: box select");
            break;
        case EditorToolId::Translate:
            add("Drag axis to move");
            add(pair("view.decrease_snap", "view.increase_snap", "snap step"));
            add(keyed("transform.space", "world/local"));
            break;
        case EditorToolId::Rotate:
            add("Drag ring to rotate");
            add(pair("view.decrease_angle_snap", "view.increase_angle_snap", "angle snap"));
            add(keyed("transform.space", "world/local"));
            break;
        case EditorToolId::AddVoxel:
        case EditorToolId::RemoveVoxel:
            add(activeTool_ == EditorToolId::AddVoxel ? "Click face to add" : "Click to remove");
            add(pair("voxel.brush_decrease", "voxel.brush_increase", "brush size"));
            add(keyed("voxel.toggle_operation", "add/remove"));
            break;
        case EditorToolId::PaintMaterial:
            add("Click to paint");
            add(pair("voxel.brush_decrease", "voxel.brush_increase", "brush size"));
            add(keyed("voxel.sample_material", "pick material"));
            break;
        case EditorToolId::Box: add("Click to add a 3x3x3 box"); break;
        case EditorToolId::Beam: add("Click to add a beam along +X"); break;
        case EditorToolId::Anchor: add("Click voxel to toggle anchor"); break;
        case EditorToolId::Scale:
            add("Drag handle to scale");
            add("Shift toggles snap");
            break;
        case EditorToolId::ScatterBrush: {
            if (brushSourceRoots_.empty() && !brushPrefab_) {
                add("Select objects or a prefab, then pick this tool");
                break;
            }
            char radius[48];
            std::snprintf(radius, sizeof radius, "Ctrl+wheel radius %.1f m", static_cast<double>(scatterSettings_.brushRadiusMeters));
            add("Drag paint");
            add("Shift-drag erase");
            add(radius);
            break;
        }
    }
    return gestures;
}

void NativeEditorController::set_active_tool(EditorToolId tool) {
    if (tool != activeTool_) {
        cancel_polygon_preview();
        if (gizmoDragging_) finish_gizmo_drag(true);
    }
    const EditorToolId previous = activeTool_;
    if (previous == EditorToolId::ScatterBrush && brushStroking_) finish_brush_stroke(false);
    activeTool_ = tool;
    close_top_level_menu();
    if (tool == EditorToolId::ScatterBrush && !brushStroking_) begin_scatter_brush_session();
    if ((previous == EditorToolId::ScatterBrush) != (tool == EditorToolId::ScatterBrush)) {
        if (textEdit_.kind == TextEditKind::ScatterSetting && !scatter_settings_panel_active()) cancel_text_edit();
        recompute_layout();  // the scatter settings panel comes and goes with the brush
    }
    if (tool != EditorToolId::ScatterBrush) {
        brushCursor_.reset();
        brushResizing_ = false;
    }
}
void NativeEditorController::set_active_material(MaterialId material) noexcept {
    if (material != kAirMaterial && materials_.find(material)) activeMaterial_ = material;
}

void NativeEditorController::begin_voxel_stroke(int x, int y) {
    voxelStrokeActive_ = true;
    ++strokeId_;
    update_hover(x, y);
    if (hoverPick_) apply_voxel_tool(*hoverPick_);
}
void NativeEditorController::continue_voxel_stroke(int x, int y) {
    update_hover(x, y);
    if (hoverPick_) apply_voxel_tool(*hoverPick_);
}
void NativeEditorController::finish_voxel_stroke() noexcept { voxelStrokeActive_ = false; }

void NativeEditorController::apply_voxel_tool(const EditorPickResult& pick) {
    const EditorObject* object = workspace_.document().find_object(pick.objectId);
    if (!object) return;
    if (object->polygon) {
        set_status("Use Polygon > component modes to edit mesh geometry or assign materials to faces",true);return;
    }
    std::unique_ptr<IEditorCommand> command;
    if (activeTool_ == EditorToolId::Anchor) {
        const bool addAnchor = !object->anchors.contains(pick.voxel);
        command = make_anchor_brush_command(workspace_.document(), pick.objectId, pick.voxel, 0, addAnchor);
    } else {
        BrushSettings settings;
        settings.shape = voxelBrushShape_;
        settings.radius = voxelBrushRadius_;
        settings.material = activeMaterial_;
        settings.strokeId = strokeId_;
        Int3 center = pick.voxel;
        switch (activeTool_) {
            case EditorToolId::AddVoxel:
                settings.operation = VoxelToolOperation::Add;
                center = add_int3(pick.voxel, pick.normal);
                break;
            case EditorToolId::RemoveVoxel:
            case EditorToolId::PaintMaterial: {
                settings.operation = activeTool_ == EditorToolId::RemoveVoxel ? VoxelToolOperation::Remove : VoxelToolOperation::Paint;
                prune_voxel_selection();
                if (!voxelSelection_.empty() && pick.objectId == voxelSelectionObject_) {
                    // With a voxel selection, Paint and Remove only touch selected voxels.
                    std::vector<VoxelChange> changes;
                    for (const Int3 cell : voxels_in_cube(*object->voxels, pick.voxel, voxelBrushRadius_)) {
                        if (!voxelSelection_.contains(cell) || !in_voxel_brush(cell, pick.voxel)) continue;
                        const MaterialId before = object->voxels->material_at(cell);
                        // Same rule as the brush command: painting with air leaves the cell alone.
                        const MaterialId after = settings.operation == VoxelToolOperation::Remove ? kAirMaterial
                            : (before == kAirMaterial || activeMaterial_ == kAirMaterial ? before : activeMaterial_);
                        if (before != after) changes.push_back({cell, before, after});
                    }
                    if (changes.empty()) { set_status("Only selected voxels can change (clear the selection to edit freely)", false, 2.0F); return; }
                    command = std::make_unique<VoxelEditCommand>(pick.objectId, std::move(changes),
                        settings.operation == VoxelToolOperation::Remove ? "Remove voxels" : "Paint material", strokeId_);
                }
                break;
            }
            case EditorToolId::Box:
                command = make_box_command(workspace_.document(), pick.objectId,
                                           add_int3(pick.voxel, {-1,-1,-1}), add_int3(pick.voxel, {1,1,1}),
                                           VoxelToolOperation::Add, activeMaterial_, false);
                break;
            case EditorToolId::Beam:
                command = make_line_command(workspace_.document(), pick.objectId, pick.voxel,
                                            add_int3(pick.voxel, {4,0,0}), VoxelToolOperation::Add,
                                            activeMaterial_, 1);
                break;
            default: return;
        }
        if (!command) command = make_brush_command(workspace_.document(), pick.objectId, center, settings);
    }
    if (const auto* edit = dynamic_cast<const VoxelEditCommand*>(command.get()); edit && edit->change_count() == 0) {
        // A click that changes nothing adds no undo step.
        set_status(object->flags.locked ? "Unlock " + object->name + " to edit its voxels" : std::string("Nothing to change here"),
                   object->flags.locked, 1.5F);
        return;
    }
    const CommandResult result = workspace_.commands().execute(workspace_.document(), std::move(command));
    set_status(result.success ? "Voxel edit applied" : result.message, !result.success, 1.5F);
}

EditorObjectBounds NativeEditorController::selection_bounds() const noexcept {
    EditorObjectBounds combined;
    for (EditorObjectId id : workspace_.selected_objects()) {
        const EditorObject* object = workspace_.document().find_object(id);
        if (!object || !object->flags.visible) continue;
        const EditorObjectBounds bounds = is_group(id) ? group_bounds(id) : object_world_bounds(*object);
        if (!bounds.valid) continue;
        if (!combined.valid) combined = bounds;
        else {
            combined.minimum = {
                std::min(combined.minimum.x, bounds.minimum.x),
                std::min(combined.minimum.y, bounds.minimum.y),
                std::min(combined.minimum.z, bounds.minimum.z)};
            combined.maximum = {
                std::max(combined.maximum.x, bounds.maximum.x),
                std::max(combined.maximum.y, bounds.maximum.y),
                std::max(combined.maximum.z, bounds.maximum.z)};
        }
    }
    return combined;
}

// --- Groups: closed by default, open to edit inside, editable pivot (ART-035/036) --------------

void NativeEditorController::prune_groups() {
    // Undo, delete or ungroup can remove a group or give it geometry; forget those.
    for (auto it = openGroups_.begin(); it != openGroups_.end();)
        it = is_group(*it) ? std::next(it) : openGroups_.erase(it);
    if (pivotEditing_) {
        const auto active = workspace_.selected_object();
        if (workspace_.selection_count() != 1 || !active || !is_group(*active)) {
            pivotEditing_ = false;
            refresh_menu_state();
        }
    }
}

bool NativeEditorController::has_own_geometry(const EditorObject& object) const {
    return (object.voxels && object.voxels->occupied_voxel_count() > 0) || object.polygon || object.subdivision ||
           object.text3d || object.gaborVolume;
}

bool NativeEditorController::is_group(EditorObjectId id) const {
    const EditorObject* object = workspace_.document().find_object(id);
    return object && !has_own_geometry(*object) && !workspace_.document().children_of(id).empty();
}

EditorObjectId NativeEditorController::viewport_selection_target(EditorObjectId id) const {
    // Walk from the root down: the first group on the way that is closed takes the click.
    std::vector<EditorObjectId> chain;
    const EditorDocument& document = workspace_.document();
    for (const EditorObject* object = document.find_object(id); object && object->parent && chain.size() < 4096;) {
        chain.push_back(*object->parent);
        object = document.find_object(*object->parent);
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        if (is_group(*it) && !openGroups_.contains(*it)) return *it;
    return id;
}

EditorObjectBounds NativeEditorController::group_bounds(EditorObjectId id) const {
    EditorObjectBounds combined;
    const EditorDocument& document = workspace_.document();
    for (const EditorObjectId member : collect_editor_object_subtree_ids(document, std::vector<EditorObjectId>{id})) {
        const EditorObject* object = document.find_object(member);
        if (!object || !object->flags.visible) continue;
        const EditorObjectBounds bounds = object_world_bounds(*object);
        if (!bounds.valid) continue;
        if (!combined.valid) { combined = bounds; continue; }
        combined.minimum = {std::min(combined.minimum.x, bounds.minimum.x), std::min(combined.minimum.y, bounds.minimum.y),
                            std::min(combined.minimum.z, bounds.minimum.z)};
        combined.maximum = {std::max(combined.maximum.x, bounds.maximum.x), std::max(combined.maximum.y, bounds.maximum.y),
                            std::max(combined.maximum.z, bounds.maximum.z)};
    }
    return combined;
}

bool NativeEditorController::open_selected_groups() {
    std::size_t opened = 0;
    for (const EditorObjectId id : workspace_.selected_objects()) {
        // A selected piece inside a closed group opens that group too.
        const EditorObjectId target = is_group(id) ? id : viewport_selection_target(id);
        if (is_group(target) && openGroups_.insert(target).second) ++opened;
    }
    if (opened == 0) {
        set_status("Select a group to open (or double-click a piece in it)", true);
        return false;
    }
    refresh_menu_state();
    set_status("Opened " + std::to_string(opened) + " group(s): clicks now reach the pieces inside (Close Group returns)");
    return true;
}

bool NativeEditorController::close_groups_around_selection() {
    // Close the nearest open group around each selected object (or the selected open group), and
    // select it.
    std::set<EditorObjectId> closed;
    const EditorDocument& document = workspace_.document();
    for (const EditorObjectId id : workspace_.selected_objects()) {
        std::optional<EditorObjectId> target;
        if (openGroups_.contains(id)) target = id;
        for (const EditorObject* object = document.find_object(id); !target && object && object->parent;) {
            if (openGroups_.contains(*object->parent)) target = *object->parent;
            object = document.find_object(*object->parent);
        }
        if (target) closed.insert(*target);
    }
    if (closed.empty() && !openGroups_.empty() && workspace_.selected_objects().empty()) {
        close_all_groups();
        return true;
    }
    if (closed.empty()) { set_status("Nothing to close: the selection is not inside an open group", true); return false; }
    for (const EditorObjectId id : closed) {
        // Closing a group also closes the groups inside it.
        for (const EditorObjectId member : collect_editor_object_subtree_ids(document, std::vector<EditorObjectId>{id}))
            openGroups_.erase(member);
    }
    workspace_.clear_selection();
    for (const EditorObjectId id : closed) workspace_.add_to_selection(viewport_selection_target(id));
    pivotEditing_ = false;
    refresh_menu_state();
    set_status("Closed " + std::to_string(closed.size()) + " group(s)");
    return true;
}

void NativeEditorController::close_all_groups() {
    openGroups_.clear();
    pivotEditing_ = false;
    refresh_menu_state();
    set_status("Closed every group");
}

bool NativeEditorController::toggle_pivot_editing() {
    if (pivotEditing_) {
        pivotEditing_ = false;
        refresh_menu_state();
        set_status("Pivot editing finished");
        return true;
    }
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) { set_status("Stop Play or Simulate before editing a pivot", true); return false; }
    const auto active = workspace_.selected_object();
    if (workspace_.selection_count() != 1 || !active || !is_group(*active)) {
        set_status("Select one group to edit its pivot", true);
        return false;
    }
    if (const EditorObject* group = workspace_.document().find_object(*active); group && group->flags.locked) {
        set_status("Unlock " + group->name + " before editing its pivot", true);
        return false;
    }
    for (const EditorObjectId child : workspace_.document().children_of(*active))
        if (const EditorObject* object = workspace_.document().find_object(child); object && object->attachment) {
            set_status("Edit Pivot needs pieces that are not socket-attached (" + object->name + " is)", true);
            return false;
        }
    if (const EditorObject* group = workspace_.document().find_object(*active); group && imported_group(*group)) {
        set_status(std::string(kImportedPivotFixed), true);
        return false;
    }
    pivotEditing_ = true;
    if (activeTool_ != EditorToolId::Translate && activeTool_ != EditorToolId::Rotate) set_active_tool(EditorToolId::Translate);
    refresh_menu_state();
    set_status("Editing the pivot: Move or Rotate moves the pivot only; Edit Pivot again (or Esc) finishes");
    return true;
}

bool NativeEditorController::set_group_pivot(bool bottomCentre) {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) { set_status("Stop Play or Simulate before editing a pivot", true); return false; }
    const auto active = workspace_.selected_object();
    if (workspace_.selection_count() != 1 || !active || !is_group(*active)) { set_status("Select one group first", true); return false; }
    const EditorObject* group = workspace_.document().find_object(*active);
    if (imported_group(*group)) {
        set_status(std::string(kImportedPivotFixed), true);
        return false;
    }
    if (group->flags.locked) { set_status("Unlock " + group->name + " before moving its pivot", true); return false; }
    const EditorObjectBounds bounds = group_bounds(*active);
    if (!bounds.valid) { set_status("The group has no visible pieces to measure", true); return false; }
    RigidTransform after = group->transform;
    after.position = {(bounds.minimum.x + bounds.maximum.x) * 0.5F,
                      bottomCentre ? bounds.minimum.y : (bounds.minimum.y + bounds.maximum.y) * 0.5F,
                      (bounds.minimum.z + bounds.maximum.z) * 0.5F};
    if (after.position.x == group->transform.position.x && after.position.y == group->transform.position.y &&
        after.position.z == group->transform.position.z) {
        // Already there: no undo step for an edit that changes nothing.
        set_status(std::string("The pivot is already at the group's ") + (bottomCentre ? "bottom centre" : "centre"));
        return false;
    }
    std::vector<ObjectTransformChange> changes{{group->id, group->transform, after}};
    // Socket-attached children would follow their parent: hold them where they are.
    for (const EditorObjectId child : workspace_.document().children_of(group->id))
        if (const EditorObject* object = workspace_.document().find_object(child); object && object->attachment) {
            const RigidTransform world = workspace_.document().world_transform(child).value_or(object->transform);
            changes.push_back({child, world, world});
        }
    const CommandResult result = workspace_.commands().execute(workspace_.document(),
        std::make_unique<TransformObjectsCommand>(std::move(changes), bottomCentre ? "Pivot to bottom centre" : "Pivot to centre"));
    set_status(result.success ? (bottomCentre ? "Pivot moved to the group's bottom centre" : "Pivot moved to the group's centre")
                              : result.message, !result.success);
    return result.success;
}

Float3 NativeEditorController::selection_pivot() const noexcept {
    if (selectionDomain_ == SelectionDomain::Polygons) return polygon_selection_centre();
    if (pivotMode_ == PivotMode::WorldOrigin) return {};
    if (pivotMode_ == PivotMode::Custom) return customPivot_;
    // A single selected group turns and moves about its own pivot.
    if (workspace_.selection_count() == 1 && workspace_.selected_object() && is_group(*workspace_.selected_object()))
        if (const EditorObject* group = workspace_.document().find_object(*workspace_.selected_object()))
            return group->transform.position;
    if (pivotMode_ == PivotMode::ActiveObject && workspace_.selected_object()) {
        if (const EditorObject* object = workspace_.document().find_object(*workspace_.selected_object()))
            return object->transform.position;
    }
    const EditorObjectBounds bounds = selection_bounds();
    if (bounds.valid) return multiply(add(bounds.minimum, bounds.maximum), 0.5F);
    if (workspace_.selected_object()) {
        if (const EditorObject* object = workspace_.document().find_object(*workspace_.selected_object()))
            return object->transform.position;
    }
    return {};
}

Float3 NativeEditorController::gizmo_axis_world(int axis) const noexcept {
    Float3 direction = axis == 1 ? Float3{1.0F, 0.0F, 0.0F} :
                       axis == 2 ? Float3{0.0F, 1.0F, 0.0F} : Float3{0.0F, 0.0F, 1.0F};
    if ((transformSpace_ == EditorTransformSpace::Local || activeTool_ == EditorToolId::Scale) && workspace_.selected_object()) {
        if (const EditorObject* object = workspace_.document().find_object(*workspace_.selected_object()))
            direction = transform_vector(object->transform, direction);
    }
    return normalize(direction);
}

std::vector<ObjectTransformChange> NativeEditorController::selection_transform_snapshot(bool includeGroupMembers) const {
    std::vector<ObjectTransformChange> changes;
    changes.reserve(workspace_.selection_count());
    std::set<EditorObjectId> seen;
    for (EditorObjectId id : workspace_.selected_objects()) {
        const EditorObject* object = workspace_.document().find_object(id);
        if (!object || !seen.insert(id).second) continue;
        changes.push_back({id, object->transform, object->transform});
    }
    if (!includeGroupMembers || pivotEditing_) return changes;
    // A selected group moves as a unit: its unattached descendants come along (attached ones
    // already follow their parent). Groups come first so parents update before their pieces.
    for (EditorObjectId id : workspace_.selected_objects()) {
        if (!is_group(id)) continue;
        for (const EditorObjectId member : collect_editor_object_subtree_ids(workspace_.document(), std::vector<EditorObjectId>{id})) {
            const EditorObject* object = workspace_.document().find_object(member);
            if (!object || object->attachment || !seen.insert(member).second) continue;
            changes.push_back({member, object->transform, object->transform});
        }
    }
    return changes;
}

void NativeEditorController::apply_transform_preview(const std::vector<ObjectTransformChange>& changes, bool after) {
    for (const ObjectTransformChange& change : changes) {
        if (EditorObject* object = workspace_.document().find_object(change.id))
            object->transform = after ? change.after : change.before;
    }
}

// Orthonormal basis (u, v) spanning the plane perpendicular to an axis: the
// rotate ring for the axis lies in this plane.
void gizmo_ring_basis(Float3 axisWorld, Float3& u, Float3& v) noexcept {
    const auto cross3 = [](Float3 a, Float3 b) noexcept {
        return Float3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    };
    const Float3 helper = std::abs(axisWorld.y) < 0.9F ? Float3{0.0F, 1.0F, 0.0F}
                                                       : Float3{1.0F, 0.0F, 0.0F};
    u = normalize(cross3(axisWorld, helper));
    v = cross3(axisWorld, u);
}

// World angle of a pointer position around a ring: intersect the pointer
// ray with the ring's plane through the pivot and measure the hit in the
// ring basis. False when the ray is nearly parallel to the plane or lands
// on the pivot, where the angle is undefined.
bool gizmo_pointer_ring_angle(const EditorCamera& camera, UiRect viewport, Float3 pivot,
                              Float3 axisWorld, Float3 basisU, Float3 basisV, int x, int y,
                              float& angleOut) noexcept {
    const ViewportRay ray = make_viewport_ray(camera, viewport, static_cast<float>(x),
                                              static_cast<float>(y));
    const float denom = dot(ray.direction, axisWorld);
    if (std::abs(denom) < 1.0e-4F) return false;
    const float t = dot(subtract(pivot, ray.origin), axisWorld) / denom;
    if (!(t > 0.0F)) return false;
    const Float3 rel = subtract(add(ray.origin, multiply(ray.direction, t)), pivot);
    if (dot(rel, rel) < 1.0e-8F) return false;
    angleOut = std::atan2(dot(rel, basisV), dot(rel, basisU));
    return true;
}

void NativeEditorController::update_placement_preview(int x, int y) {
    if (placementMode_ == PlacementTarget::NoTarget) return;
    apply_transform_preview(placementChanges_, false);
    placementPoint_.reset();
    const auto hits = pick_editor_document_all(workspace_.document(),
        make_viewport_ray(camera_, layout_.viewport, static_cast<float>(x), static_cast<float>(y)),
        10000.0F, &viewportSettings_.isolatedObjects);
    const auto it = std::find_if(hits.begin(), hits.end(), [&](const EditorPickResult& hit) {
        return !workspace_.is_selected(hit.objectId);
    });
    if (it == hits.end()) return;
    const EditorObject* target = workspace_.document().find_object(it->objectId);
    if (!target) return;
    Float3 point = add(it->worldPosition, multiply(it->worldNormal, placementOffsetMeters_));
    if (placementMode_ != PlacementTarget::Surface) {
        std::vector<Float3> candidates;
        if (placementMode_ == PlacementTarget::BoundsVertex) {
            const EditorObjectBounds bounds = object_world_bounds(*target);
            if (!bounds.valid) return;
            for (int corner = 0; corner < 8; ++corner)
                candidates.push_back({(corner & 1) ? bounds.maximum.x : bounds.minimum.x,
                    (corner & 2) ? bounds.maximum.y : bounds.minimum.y,
                    (corner & 4) ? bounds.maximum.z : bounds.minimum.z});
        } else if (placementMode_ == PlacementTarget::CollisionBoxVertex && target->flags.collisionEnabled &&
                   target->voxels && target->voxels->brick_count() > 0) {
            const auto boxes = build_brick_box_proxy(*target->voxels, brick_key_from_voxel(it->voxel));
            for (const VoxelBox& box : boxes) {
                if (it->voxel.x < box.min.x || it->voxel.y < box.min.y || it->voxel.z < box.min.z ||
                    it->voxel.x >= box.maxExclusive.x || it->voxel.y >= box.maxExclusive.y ||
                    it->voxel.z >= box.maxExclusive.z) continue;
                for (int corner = 0; corner < 8; ++corner)
                    candidates.push_back(voxel_to_world(target->transform, multiply(target->voxelScale,target->voxelSizeMeters),Float3{
                        static_cast<float>((corner & 1) ? box.maxExclusive.x : box.min.x),
                        static_cast<float>((corner & 2) ? box.maxExclusive.y : box.min.y),
                        static_cast<float>((corner & 4) ? box.maxExclusive.z : box.min.z)}));
            }
        } else if (target->voxels && target->voxels->brick_count() > 0 && target->voxelSizeMeters > 0.0F) {
            const float size = target->voxelSizeMeters;
            std::array<Float3, 8> corners{};
            for (int corner = 0; corner < 8; ++corner) {
                corners[static_cast<std::size_t>(corner)] = transform_point(target->transform,
                    multiply_components(Float3{(static_cast<float>(it->voxel.x) + ((corner & 1) ? 1.0F : 0.0F)) * size,
                           (static_cast<float>(it->voxel.y) + ((corner & 2) ? 1.0F : 0.0F)) * size,
                           (static_cast<float>(it->voxel.z) + ((corner & 4) ? 1.0F : 0.0F)) * size},target->voxelScale));
            }
            if (placementMode_ == PlacementTarget::VoxelCorner) candidates.assign(corners.begin(), corners.end());
            else if (placementMode_ == PlacementTarget::VoxelEdge) {
                for (int a = 0; a < 8; ++a)
                    for (int b = a + 1; b < 8; ++b)
                        if (std::popcount(static_cast<unsigned>(a ^ b)) == 1)
                            candidates.push_back(multiply(add(corners[static_cast<std::size_t>(a)],
                                corners[static_cast<std::size_t>(b)]), 0.5F));
            } else if (placementMode_ == PlacementTarget::VoxelFace) {
                for (int axis = 0; axis < 3; ++axis)
                    for (int side = 0; side < 2; ++side) {
                        Float3 center{};
                        for (int corner = 0; corner < 8; ++corner)
                            if (((corner >> axis) & 1) == side)
                                center = add(center, corners[static_cast<std::size_t>(corner)]);
                        candidates.push_back(multiply(center, 0.25F));
                    }
            }
        }
        if (candidates.empty()) return;
        point = *std::min_element(candidates.begin(), candidates.end(), [&](Float3 a, Float3 b) {
            return length_squared(subtract(a, it->worldPosition)) < length_squared(subtract(b, it->worldPosition));
        });
    }
    placementPoint_ = point;
    const Float3 delta = subtract(point, placementPivot_);
    for (ObjectTransformChange& change : placementChanges_) {
        change.after = change.before;
        change.after.position = add(change.before.position, delta);
        if (placementMode_ == PlacementTarget::Surface && alignToSurfaceNormal_) {
            const Float3 n = normalize(it->worldNormal);
            const Float3 up = rotate(change.before.rotation, {0.0F, 1.0F, 0.0F});
            const float d = dot(up, n);
            const Float3 cross{up.y * n.z - up.z * n.y, up.z * n.x - up.x * n.z,
                               up.x * n.y - up.y * n.x};
            const Float3 fallback = std::abs(up.x) < 0.9F ? Float3{1.0F, 0.0F, 0.0F}
                                                      : Float3{0.0F, 0.0F, 1.0F};
            const Float3 oppositeAxis{up.y * fallback.z - up.z * fallback.y,
                                      up.z * fallback.x - up.x * fallback.z,
                                      up.x * fallback.y - up.y * fallback.x};
            const Quaternion alignment = d < -0.999F ? quaternion_from_axis_angle(oppositeAxis, 3.14159265F)
                : normalize(Quaternion{cross.x, cross.y, cross.z, 1.0F + d});
            change.after.rotation = normalize(multiply(alignment, change.before.rotation));
        }
    }
    apply_transform_preview(placementChanges_, true);
}

void NativeEditorController::finish_placement(bool cancel) {
    if (placementMode_ == PlacementTarget::NoTarget) return;
    apply_transform_preview(placementChanges_, false);
    if (!cancel && placementPoint_) {
        const CommandResult result = workspace_.commands().execute(workspace_.document(),
            std::make_unique<TransformObjectsCommand>(placementChanges_, "Place objects"));
        set_status(result.success ? "Placed selection" : result.message, !result.success);
    } else set_status("Placement canceled");
    placementChanges_.clear();
    placementPoint_.reset();
    placementMode_ = PlacementTarget::NoTarget;
}

std::vector<GizmoScreenAxis> NativeEditorController::gizmo_axes() const {
    std::vector<GizmoScreenAxis> axes;
    if (voxelSlice_.active() || (selectionDomain_==SelectionDomain::Polygons && polygonSelection_.components.empty())) return axes;
    if (selectionDomain_ == SelectionDomain::Voxels) {
        // Voxel domain: only Move has a gizmo, at the voxel selection, along the object's grid.
        if (activeTool_ != EditorToolId::Translate || voxelSelection_.empty()) return axes;
        const Float3 origin = voxel_selection_centre();
        const float distance = std::max(0.5F, length(subtract(camera_.position, origin)) * 0.12F);
        const ScreenPoint start = project_world_to_screen(camera_, layout_.viewport, origin);
        for (int axis = 1; axis <= 3; ++axis)
            axes.push_back({axis, start, project_world_to_screen(camera_, layout_.viewport,
                                                                 add(origin, multiply(voxel_move_axis_world(axis), distance)))});
        return axes;
    }
    if ((activeTool_ != EditorToolId::Translate && activeTool_ != EditorToolId::Rotate &&
         activeTool_ != EditorToolId::Scale) ||
        workspace_.selected_objects().empty()) return axes;
    const Float3 origin = gizmoDragging_ ? gizmoPivot_ : selection_pivot();
    const float distance = std::max(0.5F, length(subtract(camera_.position, origin)) * 0.12F);
    for (int axis = 1; axis <= 3; ++axis) {
        const ScreenPoint start = project_world_to_screen(camera_, layout_.viewport, origin);
        const ScreenPoint end = project_world_to_screen(camera_, layout_.viewport,
                                                         add(origin, multiply(gizmo_axis_world(axis), distance)));
        axes.push_back({axis, start, end});
    }
    return axes;
}

std::vector<GizmoScreenRing> NativeEditorController::gizmo_rings() const {
    std::vector<GizmoScreenRing> rings;
    if (voxelSlice_.active() || (selectionDomain_==SelectionDomain::Polygons && polygonSelection_.components.empty())) return rings;
    if (selectionDomain_ == SelectionDomain::Voxels) return rings;
    if (activeTool_ != EditorToolId::Rotate || workspace_.selected_objects().empty()) return rings;
    const Float3 origin = gizmoDragging_ ? gizmoPivot_ : selection_pivot();
    const float radius = std::max(0.5F, length(subtract(camera_.position, origin)) * 0.12F);
    constexpr int kSegments = 40;
    for (int axis = 1; axis <= 3; ++axis) {
        const Float3 axisWorld = gizmo_axis_world(axis);
        Float3 u{};
        Float3 v{};
        gizmo_ring_basis(axisWorld, u, v);
        GizmoScreenRing ring;
        ring.axis = axis;
        ring.points.reserve(kSegments);
        for (int i = 0; i < kSegments; ++i) {
            const float theta = static_cast<float>(i) * 6.283185307179586F /
                                static_cast<float>(kSegments);
            const Float3 point = add(origin, add(multiply(u, std::cos(theta) * radius),
                                                 multiply(v, std::sin(theta) * radius)));
            ring.points.push_back(project_world_to_screen(camera_, layout_.viewport, point));
        }
        rings.push_back(std::move(ring));
    }
    return rings;
}

int NativeEditorController::hit_test_gizmo_axis(int x, int y) const {
    int result = 0;
    float best = 10.0F * kLogicalLayoutScale;
    for (const GizmoScreenAxis& axis : gizmo_axes()) {
        if (!axis.start.visible || !axis.end.visible) continue;
        const float distance = point_line_distance(static_cast<float>(x), static_cast<float>(y),
                                                   axis.start.x, axis.start.y, axis.end.x, axis.end.y);
        if (distance < best) { best = distance; result = axis.axis; }
    }
    if (activeTool_ == EditorToolId::Rotate) {
        for (const GizmoScreenRing& ring : gizmo_rings()) {
            const std::vector<ScreenPoint>& points = ring.points;
            for (std::size_t i = 0; i < points.size(); ++i) {
                const ScreenPoint& a = points[i];
                const ScreenPoint& b = points[(i + 1U) % points.size()];
                if (!a.visible || !b.visible) continue;
                const float distance = point_line_distance(
                    static_cast<float>(x), static_cast<float>(y), a.x, a.y, b.x, b.y);
                if (distance < best) { best = distance; result = ring.axis; }
            }
        }
    }
    return result;
}

void NativeEditorController::begin_gizmo_drag(int x, int y, std::uint32_t modifiers) {
    if (workspace_.selected_objects().empty()) return;
    if (selectionDomain_ == SelectionDomain::Polygons && polygonSelection_.components.empty()) return;
    gizmoAxis_ = hit_test_gizmo_axis(x, y);
    if (gizmoAxis_ == 0) return;
    for (EditorObjectId id : workspace_.selected_objects()) {
        const EditorObject* object = workspace_.document().find_object(id);
        if (!object || object->flags.locked) {
            set_status("Unlock all selected objects before transforming them", true);
            gizmoAxis_ = 0;
            return;
        }
        if (activeTool_ == EditorToolId::Scale && is_group(id)) {
            set_status("Scale works on pieces: double-click the group (or Edit > Open Group) and select them", true);
            gizmoAxis_ = 0;
            return;
        }
        if (is_group(id) && !pivotEditing_) {
            for (const EditorObjectId member : collect_editor_object_subtree_ids(workspace_.document(), std::vector<EditorObjectId>{id}))
                if (const EditorObject* piece = workspace_.document().find_object(member); piece && piece->flags.locked) {
                    set_status("Unlock " + piece->name + " (in " + object->name + ") before moving the group", true);
                    gizmoAxis_ = 0;
                    return;
                }
        }
        if (activeTool_ == EditorToolId::Scale && ((!object->polygon && (!object->voxels || object->voxels->brick_count() == 0)) ||
            object->text3d || object->gaborVolume || !std::isfinite(object->voxelSizeMeters) ||
            object->voxelSizeMeters < 0.001F || object->voxelSizeMeters > 10.0F || !valid_voxel_metric(object->voxelScale))) {
            set_status("Scale requires polygon or occupied voxel geometry", true);
            gizmoAxis_ = 0;
            return;
        }
        if (activeTool_ == EditorToolId::Scale && (modifiers & 2U) != 0U && object->polygon) {
            // Ctrl-scale resamples voxels; polygon geometry has no cells to resample.
            set_status("Ctrl resamples voxels; release Ctrl to scale polygon geometry", true);
            gizmoAxis_ = 0;
            return;
        }
    }
    if (selectionDomain_ == SelectionDomain::Polygons) {
        close_previews_for_voxel_edit("polygon transform");
        const auto* object=workspace_.document().find_object(polygonSelection_.object);
        if (!object || !object->polygon) return;
        polygonDragBefore_=*object->polygon;
    }
    gizmoDragging_ = true;
    gizmoStartX_ = x;
    gizmoStartY_ = y;
    gizmoLastX_ = x; gizmoLastY_ = y; gizmoLastModifiers_ = modifiers;
    gizmoPivot_ = selection_pivot();
    gizmoChanges_ = selection_transform_snapshot();
    gizmoSizeChanges_.clear();
    gizmoResampleScale_ = (modifiers & 2U) != 0U;
    if (activeTool_ == EditorToolId::Scale && !gizmoResampleScale_ && selectionDomain_ != SelectionDomain::Polygons) {
        for (const auto& change : gizmoChanges_) {
            const auto* object = workspace_.document().find_object(change.id);
            gizmoSizeChanges_.push_back({change.id, change.before, change.before,
                object->voxelSizeMeters, object->voxelSizeMeters, object->voxelScale, object->voxelScale});
        }
    }
    transformNumeric_.clear();
    scalePreviewBounds_.reset();
    scaleFactors_ = {1.0F, 1.0F, 1.0F};
    gizmoDragPlane_ = false;
    gizmoDragWorldAccum_ = 0.0F;
    if (activeTool_ == EditorToolId::Rotate) {
        const Float3 axisWorld = gizmo_axis_world(gizmoAxis_);
        gizmo_ring_basis(axisWorld, gizmoDragBasisU_, gizmoDragBasisV_);
        float angle = 0.0F;
        if (gizmo_pointer_ring_angle(camera_, layout_.viewport, gizmoPivot_, axisWorld,
                                     gizmoDragBasisU_, gizmoDragBasisV_, x, y, angle)) {
            gizmoDragWorldAngle_ = angle;
            gizmoDragPlane_ = true;
        }
    }
}

void NativeEditorController::update_gizmo_drag(int x, int y, std::uint32_t modifiers) {
    if (!gizmoDragging_ || gizmoChanges_.empty()) return;
    gizmoLastX_ = x;
    gizmoLastY_ = y;
    gizmoLastModifiers_ = modifiers;
    if (activeTool_ == EditorToolId::Rotate && gizmoDragPlane_) {
        float angle = 0.0F;
        if (gizmo_pointer_ring_angle(camera_, layout_.viewport, gizmoPivot_,
                                     gizmo_axis_world(gizmoAxis_), gizmoDragBasisU_,
                                     gizmoDragBasisV_, x, y, angle)) {
            const float delta = std::atan2(std::sin(angle - gizmoDragWorldAngle_),
                                           std::cos(angle - gizmoDragWorldAngle_));
            gizmoDragWorldAccum_ += delta;
            gizmoDragWorldAngle_ = angle;
        }
    }
    const auto axes = gizmo_axes();
    if (gizmoAxis_ < 1 || gizmoAxis_ > static_cast<int>(axes.size())) return;
    const GizmoScreenAxis& screenAxis = axes[static_cast<std::size_t>(gizmoAxis_ - 1)];
    const float dx = screenAxis.end.x - screenAxis.start.x;
    const float dy = screenAxis.end.y - screenAxis.start.y;
    const float screenLength = std::max(1.0F, std::hypot(dx, dy));
    const float projectedDelta = ((static_cast<float>(x - gizmoStartX_) * dx) +
                                  (static_cast<float>(y - gizmoStartY_) * dy)) / screenLength;
    const Float3 worldAxis = gizmo_axis_world(gizmoAxis_);
    const EditorPreferences& preferences = workspace_.preferences();
    const bool temporaryOverride = (modifiers & 1U) != 0U;
    std::optional<float> numeric;
    if (!transformNumeric_.empty() && transformNumeric_ != "-" && transformNumeric_ != ".") {
        char* end = nullptr;
        const float parsed = std::strtof(transformNumeric_.c_str(), &end);
        if (end && *end == '\0' && std::isfinite(parsed)) numeric = parsed;
    }
    if (polygonDragBefore_) {
        auto* object=workspace_.document().find_object(polygonSelection_.object);
        if (!object || !object->polygon) { finish_gizmo_drag(true); return; }
        *object->polygon=*polygonDragBefore_;
        Float3 translation{}, scale{1,1,1}; Quaternion rotation{};
        if (activeTool_==EditorToolId::Scale) {
            float factor=std::clamp(numeric.value_or(1.0F+projectedDelta*.01F),.1F,10.0F);
            if (!numeric && preferences.scaleSnapEnabled != temporaryOverride)
                factor=std::max(.1F,1.0F+std::round((factor-1.0F)/preferences.scaleSnapStep)*preferences.scaleSnapStep);
            if (modifiers & 4U) scale={factor,factor,factor};
            else if (gizmoAxis_==1) scale.x=factor; else if (gizmoAxis_==2) scale.y=factor; else scale.z=factor;
        } else if (activeTool_==EditorToolId::Rotate) {
            const float step=preferences.rotateSnapDegrees*.01745329252F;
            float angle=numeric?*numeric*.01745329252F:gizmoDragPlane_?gizmoDragWorldAccum_:projectedDelta*.012F;
            if (!numeric && preferences.rotateSnapEnabled != temporaryOverride) angle=std::round(angle/step)*step;
            rotation=quaternion_from_axis_angle(worldAxis,angle);
        } else {
            const float depth=std::max(.1F,length(subtract(camera_.position,gizmoPivot_)));
            float delta=numeric.value_or(projectedDelta*2*depth*std::tan(camera_.verticalFovRadians*.5F)/float(std::max(1,layout_.viewport.height)));
            if (!numeric && preferences.translateSnapEnabled != temporaryOverride) {
                const float step=preferences.translateSnapMeters;
                delta=preferences.absoluteGridSnap?std::round((dot(gizmoPivot_,worldAxis)+delta)/step)*step-dot(gizmoPivot_,worldAxis):std::round(delta/step)*step;
            }
            translation=multiply(worldAxis,delta);
        }
        (void)transform_polygon_components(translation,rotation,scale,gizmoPivot_);
        return;
    }
    if (activeTool_ == EditorToolId::Scale) {
        float factor = numeric.value_or(1.0F + projectedDelta * 0.01F);
        factor = std::clamp(factor, 0.1F, 10.0F);
        if (!numeric && preferences.scaleSnapEnabled != temporaryOverride)
            factor = std::max(0.1F, 1.0F + std::round((factor - 1.0F) / preferences.scaleSnapStep) * preferences.scaleSnapStep);
        if (!gizmoResampleScale_) {
            scaleFactors_ = {1,1,1};
            const bool uniform = (modifiers & 4U) != 0U;
            if (uniform) scaleFactors_ = {factor,factor,factor};
            else if (gizmoAxis_ == 1) scaleFactors_.x = factor;
            else if (gizmoAxis_ == 2) scaleFactors_.y = factor;
            else scaleFactors_.z = factor;
            float minimum = 0.1F, maximum = 10.0F;
            for (const auto& change : gizmoSizeChanges_) {
                const Float3 spacing = multiply(*change.beforeAxisScale, change.beforeVoxelSize);
                const float values[] = {spacing.x,spacing.y,spacing.z};
                for (int axis=0; axis<3; ++axis) if (uniform || axis+1 == gizmoAxis_) {
                    minimum = std::max(minimum,0.001F/values[axis]);
                    maximum = std::min(maximum,10.0F/values[axis]);
                }
            }
            // Invalid imported metrics are rejected at drag start; never pass reversed clamp bounds.
            if (minimum > maximum) { set_status("Cell dimensions exceed the scale limits",true); return; }
            factor = std::clamp(factor,minimum,maximum);
            scaleFactors_ = {1,1,1};
            if (uniform) scaleFactors_ = {factor,factor,factor};
            else if (gizmoAxis_ == 1) scaleFactors_.x = factor;
            else if (gizmoAxis_ == 2) scaleFactors_.y = factor;
            else scaleFactors_.z = factor;
            const auto* primary = workspace_.document().find_object(*workspace_.selected_object());
            const Quaternion frame = primary ? primary->transform.rotation : Quaternion{};
            for (auto& change : gizmoSizeChanges_) {
                change.after = change.before;
                const Float3 offset = rotate(conjugate(frame),subtract(change.before.position,gizmoPivot_));
                change.after.position = add(gizmoPivot_,rotate(frame,multiply_components(offset,scaleFactors_)));
                change.afterAxisScale = multiply_components(*change.beforeAxisScale,scaleFactors_);
                if (auto* object = workspace_.document().find_object(change.id)) {
                    object->voxelScale = *change.afterAxisScale;
                    object->transform = change.after;
                }
            }
            set_status("Continuous local-axis scale: " + std::to_string(factor).substr(0,5) + "x (Alt: uniform)",false,1);
            return;
        }
        scaleFactors_ = {1.0F, 1.0F, 1.0F};
        if ((modifiers & 4U) != 0U) scaleFactors_ = {factor, factor, factor};
        else if (gizmoAxis_ == 1) scaleFactors_.x = factor;
        else if (gizmoAxis_ == 2) scaleFactors_.y = factor;
        else scaleFactors_.z = factor;
        const EditorObjectBounds bounds = selection_bounds();
        if (bounds.valid) {
            auto scaled = [&](Float3 p) {
                const Float3 d = subtract(p, gizmoPivot_);
                return add(gizmoPivot_, {d.x * scaleFactors_.x, d.y * scaleFactors_.y, d.z * scaleFactors_.z});
            };
            scalePreviewBounds_ = {scaled(bounds.minimum), scaled(bounds.maximum), true};
        }
        set_status("Voxel resample preview: " + std::to_string(factor).substr(0, 5) + "x (Alt: uniform)", false, 1.0F);
        return;
    }
    for (ObjectTransformChange& change : gizmoChanges_) {
        change.after = change.before;
        if (activeTool_ == EditorToolId::Rotate) {
            constexpr float degreesToRadians = 0.017453292519943295F;
            const float snapRadians = preferences.rotateSnapDegrees * degreesToRadians;
            const float rawAngle = numeric ? *numeric * degreesToRadians
                                 : gizmoDragPlane_ ? gizmoDragWorldAccum_
                                                   : projectedDelta * 0.012F;
            const float angle = (!numeric && preferences.rotateSnapEnabled != temporaryOverride)
                ? std::round(rawAngle / snapRadians) * snapRadians : rawAngle;
            const Quaternion delta = quaternion_from_axis_angle(worldAxis, angle);
            change.after.position = add(gizmoPivot_, rotate(delta, subtract(change.before.position, gizmoPivot_)));
            change.after.rotation = normalize(multiply(delta, change.before.rotation));
        } else {
            const float depth = std::max(0.1F, length(subtract(camera_.position, gizmoPivot_)));
            const float worldPerPixel = 2.0F * depth * std::tan(camera_.verticalFovRadians * 0.5F) /
                                        static_cast<float>(std::max(1, layout_.viewport.height));
            float worldDelta = numeric.value_or(projectedDelta * worldPerPixel);
            if (!numeric && preferences.translateSnapEnabled != temporaryOverride) {
                const float snapMeters = preferences.translateSnapMeters;
                if (preferences.absoluteGridSnap) {
                    const float coordinate = dot(change.before.position, worldAxis) + worldDelta;
                    worldDelta = std::round(coordinate / snapMeters) * snapMeters -
                                 dot(change.before.position, worldAxis);
                } else worldDelta = std::round(worldDelta / snapMeters) * snapMeters;
            }
            change.after.position = add(change.before.position, multiply(worldAxis, worldDelta));
        }
    }
    apply_transform_preview(gizmoChanges_, true);
}

void NativeEditorController::finish_gizmo_drag(bool cancel) {
    if (!gizmoDragging_) return;
    if (polygonDragBefore_) {
        auto* object=workspace_.document().find_object(polygonSelection_.object);
        if (object && object->polygon) {
            auto mesh=*object->polygon; *object->polygon=*polygonDragBefore_;
            if (!cancel && mesh.contentHash!=polygonDragBefore_->contentHash) {
                const auto before=capture_selection_snapshot();
                const auto result=workspace_.commands().execute(workspace_.document(),std::make_unique<ReplacePolygonCommand>(
                    object->id,*polygonDragBefore_,std::move(mesh),"Transform polygon components"));
                if (result.success) workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot());
                set_status(result.success?"Transformed polygon components":result.message,!result.success);
            }
        }
        polygonDragBefore_.reset(); gizmoDragging_=false; gizmoAxis_=0; gizmoChanges_.clear();
        gizmoSizeChanges_.clear(); transformNumeric_.clear(); return;
    }
    for (const auto& change : gizmoSizeChanges_) {
        if (auto* object=workspace_.document().find_object(change.id)) {
            object->voxelSizeMeters=change.beforeVoxelSize;
            if (change.beforeAxisScale) object->voxelScale=*change.beforeAxisScale;
            object->transform=change.before;
        }
    }
    apply_transform_preview(gizmoChanges_, false);
    if (!cancel && !gizmoChanges_.empty()) {
        if (activeTool_ == EditorToolId::Scale) {
            if (!gizmoResampleScale_) {
                if (std::abs(scaleFactors_.x-1.0F)>0.000001F || std::abs(scaleFactors_.y-1.0F)>0.000001F || std::abs(scaleFactors_.z-1.0F)>0.000001F) {
                    const auto result=workspace_.commands().execute(workspace_.document(),
                        std::make_unique<ScaleVoxelSizeCommand>(gizmoSizeChanges_,"Scale objects continuously"));
                    set_status(result.success ? "Scaled geometry without resampling" : result.message,!result.success);
                }
            } else {
                if (std::abs(scaleFactors_.x - 1.0F) > 0.0001F ||
                    std::abs(scaleFactors_.y - 1.0F) > 0.0001F ||
                    std::abs(scaleFactors_.z - 1.0F) > 0.0001F) {
                    auto compound = std::make_unique<CompoundCommand>("Resample selected voxel objects");
                    bool valid = true;
                    for (ObjectTransformChange& change : gizmoChanges_) {
                        const EditorObject* object = workspace_.document().find_object(change.id);
                        auto command = make_rescale_voxel_object_command(workspace_.document(), change.id, scaleFactors_);
                        if (!command || !object) { valid = false; break; }
                        compound->add(std::move(command));
                        const auto occupied = object->voxels->occupied_bounds();
                        const Float3 localCenter = occupied.valid ? Float3{
                            (static_cast<float>(occupied.minimum.x) + static_cast<float>(occupied.maximum.x) + 1.0F) * object->voxelSizeMeters * 0.5F,
                            (static_cast<float>(occupied.minimum.y) + static_cast<float>(occupied.maximum.y) + 1.0F) * object->voxelSizeMeters * 0.5F,
                            (static_cast<float>(occupied.minimum.z) + static_cast<float>(occupied.maximum.z) + 1.0F) * object->voxelSizeMeters * 0.5F}
                            : Float3{};
                        const auto scaled_center = [&](int minimum, int maximum, float factor) {
                            const double sourceCenter = 0.5 * (static_cast<double>(minimum) + maximum + 1.0);
                            const double dimension = static_cast<double>(maximum) - minimum + 1.0;
                            const double targetDimension = std::max(1.0, static_cast<double>(std::llround(dimension * factor)));
                            return static_cast<float>(std::floor(sourceCenter - targetDimension * 0.5) +
                                                      targetDimension * 0.5) * object->voxelSizeMeters;
                        };
                        const Float3 newLocalCenter = occupied.valid ? Float3{
                            scaled_center(occupied.minimum.x, occupied.maximum.x, scaleFactors_.x),
                            scaled_center(occupied.minimum.y, occupied.maximum.y, scaleFactors_.y),
                            scaled_center(occupied.minimum.z, occupied.maximum.z, scaleFactors_.z)}
                            : Float3{};
                        const Float3 rotatedCenter = rotate(change.before.rotation, multiply_components(localCenter,object->voxelScale));
                        const Float3 centerWorld = add(change.before.position, rotatedCenter);
                        const Float3 offset = subtract(centerWorld, gizmoPivot_);
                        const Float3 scaledCenter = add(gizmoPivot_, {
                            offset.x * scaleFactors_.x, offset.y * scaleFactors_.y, offset.z * scaleFactors_.z});
                        change.after.position = subtract(scaledCenter, rotate(change.before.rotation, multiply_components(newLocalCenter,object->voxelScale)));
                    }
                    if (valid) {
                        compound->add(std::make_unique<TransformObjectsCommand>(gizmoChanges_, "Scale positions"));
                        const CommandResult result = workspace_.commands().execute(workspace_.document(), std::move(compound));
                        set_status(result.success ? "Resampled selected voxels" : result.message, !result.success);
                    } else set_status("Voxel resample exceeds the supported range", true);
                }
            }
        } else {
            const std::string label = activeTool_ == EditorToolId::Rotate ? "Rotate objects" : "Move objects";
            const CommandResult result = workspace_.commands().execute(
                workspace_.document(), std::make_unique<TransformObjectsCommand>(gizmoChanges_, label));
            set_status(result.success ? label : result.message, !result.success);
        }
    }
    gizmoDragging_ = false;
    gizmoAxis_ = 0;
    gizmoChanges_.clear();
    gizmoSizeChanges_.clear();
    scalePreviewBounds_.reset();
    transformNumeric_.clear();
}

namespace {
bool same_transform(const RigidTransform& a, const RigidTransform& b);  // numeric fields, below
}  // namespace

bool NativeEditorController::dispatch_action(std::string_view actionId) {
    if (longJob_ || importOptions_.open) {
        // Only the viewport shading modes leave the scene alone while a long job runs.
        static constexpr std::array<std::string_view, 8> kSafeWhileBusy{
            "view.lit", "view.unlit", "view.wireframe", "view.material_ids", "view.normals",
            "view.lighting_only", "view.overdraw", "view.voxel_debug"};
        const bool safe = std::find(kSafeWhileBusy.begin(), kSafeWhileBusy.end(), actionId) != kSafeWhileBusy.end();
        if (longJob_ && !safe) { set_status(long_job_busy_reason(), true, 4.0F); return false; }
        if (importOptions_.open && !safe) close_import_options();
    }
    if (polygonPreviewBefore_ && !actionId.starts_with("view.") && actionId != "polygon.preview_apply" && actionId != "polygon.preview_cancel" &&
        actionId != "subdiv.preview_apply" && actionId != "subdiv.preview_cancel") {
        set_status("Apply or cancel the polygon preview first",true); return false;
    }
    if(mesh_authoring_dragging() && !actionId.starts_with("view.")) {
        set_status("Apply or cancel the mesh workspace preview first",true);return false;
    }
    if (voxel_authoring_active() && !actionId.starts_with("view.") &&
        actionId != "voxel.authoring_apply" && actionId != "voxel.authoring_cancel" &&
        actionId != "subdiv.conversion_apply" && actionId != "subdiv.conversion_cancel") {
        set_status("Apply or cancel the voxel/conversion preview first",true); return false;
    }
    if (actionId == "voxel.authoring_apply" || actionId == "subdiv.conversion_apply") return apply_voxel_authoring_preview();
    if (actionId == "voxel.authoring_cancel" || actionId == "subdiv.conversion_cancel") {
        if (!voxel_authoring_active()) { set_status("No voxel/conversion preview is open",true); return false; }
        cancel_voxel_authoring_preview(); refresh_menu_state(); set_status("Preview cancelled; scene unchanged"); return true;
    }
    if (actionId.starts_with("inspector.field.")) {
        if (!contextNumberField_) { set_status("Right-click a value first", true); return false; }
        const auto [field, axis] = *contextNumberField_;
        if (actionId == "inspector.field.reset") return reset_number_field(field, axis);
        if (actionId == "inspector.field.reset_row") {
            // One undo step for the three axes.
            const auto ids = number_field_objects(field);
            if (ids.empty()) { set_status("Nothing selected", true); return false; }
            if (const std::string refusal = number_field_refusal(field); !refusal.empty()) { set_status(refusal, true); return false; }
            std::vector<ObjectTransformChange> changes;
            bool changed = false;
            for (const EditorObjectId id : ids) {
                const EditorObject* object = workspace_.document().find_object(id);
                ObjectTransformChange change{id, object->transform, object->transform};
                if (field == NumberFieldId::Position) change.after.position = {};
                else change.after.rotation = {};
                changed = changed || !same_transform(change.before, change.after);
                changes.push_back(change);
            }
            const std::string label = field == NumberFieldId::Position ? "Reset Position" : "Reset Rotation";
            if (!changed) { set_status(label + ": already reset"); return true; }
            const auto selection = capture_selection_snapshot();
            const CommandResult result = workspace_.commands().execute(workspace_.document(),
                std::make_unique<TransformObjectsCommand>(std::move(changes), label));
            if (result.success) workspace_.commands().set_last_selection_transition(selection, selection);
            set_status(result.success ? label : result.message, !result.success);
            return result.success;
        }
        if (actionId == "inspector.field.copy") {
            const std::string value = number_field_text(field, axis);
            if (value.empty() || value == "\xE2\x80\x94") { set_status("The selected objects differ; select one to copy its value", true); return false; }
            numberFieldClipboard_ = value;
            set_status("Copied " + value);
            return true;
        }
        if (actionId == "inspector.field.paste") {
            if (numberFieldClipboard_.empty()) { set_status("Nothing copied yet", true); return false; }
            return set_number_field(field, axis, numberFieldClipboard_, "Paste");
        }
    }
    if (actionId.starts_with("selection.pick.")) {
        EditorObjectId id{};
        const auto value = actionId.substr(std::string_view("selection.pick.").size());
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), id);
        if (error != std::errc{} || end != value.data() + value.size() ||
            !workspace_.document().find_object(id)) return false;
        workspace_.select_object(id);
        return true;
    }
    // Selection, clipboard, play-session, and authoring-panel state can change outside menu
    // interaction. Re-evaluate availability at the command boundary so shortcuts, automation,
    // context menus, and top-level menus all enforce the same current-state contract.
    refresh_menu_state();
    if (const MenuAction* action = std::as_const(workspace_.menus()).find(actionId)) {
        if (!action->enabled) {
            set_status(action->disabledReason.empty() ? "That command is not available in the current context"
                                                       : action->disabledReason, true, 6.0F);
            return false;
        }
        record_command_use(actionId);
    }
    if (playSession_.active()) {
        const bool sessionSafe =
            actionId.starts_with("play.") || actionId.starts_with("physics.") ||
            actionId.starts_with("camera.") || actionId.starts_with("view.") ||
            actionId.starts_with("window.") || actionId.starts_with("help.");
        if (!sessionSafe) {
            set_status("Stop Play/Simulate before changing authored content", true);
            return false;
        }
    }

    const auto setSessionSetting = [&](std::string_view id, SettingValue value) {
        std::string error;
        if (!workspace_.settings().set(SettingScope::Session, id, std::move(value), &error)) {
            set_status(error, true);
            return false;
        }
        apply_settings_to_runtime();
        return true;
    };
    const auto toggleSessionSetting = [&](std::string_view id) {
        const SettingValue current = workspace_.settings().value(id);
        const auto* enabled = std::get_if<bool>(&current);
        return enabled && setSessionSetting(id, !*enabled);
    };
    const auto openProjectCategory = [&](std::string category) {
        open_settings(SettingScope::Project, std::move(category));
        return true;
    };

    const auto flyStep = [&](Float3 direction) {
        const SettingValue speedValue = workspace_.settings().value("camera.fly_speed");
        const SettingValue boostValue = workspace_.settings().value("camera.boost_multiplier");
        const float speed = std::get_if<double>(&speedValue)
            ? static_cast<float>(std::get<double>(speedValue)) : workspace_.preferences().cameraSpeed;
        const float boost = std::get_if<double>(&boostValue)
            ? static_cast<float>(std::get<double>(boostValue)) : 4.0F;
        const bool boosted = false;
        remember_camera_position();
        fly_camera(camera_, direction, 1.0F / 30.0F, speed * (boosted ? boost : 1.0F));
        return true;
    };

    if (actionId == "help.shortcuts") { open_shortcut_editor(); return true; }
    if (actionId == "view.advanced_menus") {
        showAdvancedMenus_ = !showAdvancedMenus_;
        refresh_menu_state();
        persist_menu_state();
        set_status(showAdvancedMenus_ ? "Advanced menu commands shown" : "Advanced menu commands hidden; use Command Center to find them");
        return true;
    }
    if (actionId == "help.reset_command_history") {
        favoriteCommandIds_.clear();
        recentCommandIds_.clear();
        persist_menu_state();
        set_status("Command favorites and recent history cleared");
        return true;
    }
    if (actionId == "transform.select") { set_active_tool(EditorToolId::Select); return true; }
    if (actionId == "transform.scale") {
        set_active_tool(EditorToolId::Scale);
        set_status("Scale: local-axis drag keeps cells; Alt during drag: uniform; Ctrl at start: resample");
        return true;
    }
    if (actionId == "transform.universal") { set_active_tool(EditorToolId::Translate); return true; }
    if (actionId == "camera.fly_forward") return flyStep({0.0F,0.0F,1.0F});
    if (actionId == "camera.fly_backward") return flyStep({0.0F,0.0F,-1.0F});
    if (actionId == "camera.fly_left") return flyStep({-1.0F,0.0F,0.0F});
    if (actionId == "camera.fly_right") return flyStep({1.0F,0.0F,0.0F});
    if (actionId == "camera.fly_down") return flyStep({0.0F,-1.0F,0.0F});
    if (actionId == "camera.fly_up") return flyStep({0.0F,1.0F,0.0F});
    if (actionId == "view.ui_zoom_in") return step_ui_zoom(1);
    if (actionId == "view.ui_zoom_out") return step_ui_zoom(-1);
    if (actionId == "view.ui_zoom_reset") return step_ui_zoom(0);
    if (actionId.starts_with("view.keyboard_keys_")) {
        int keys = 0;
        const std::string_view digits = actionId.substr(std::string_view("view.keyboard_keys_").size());
        const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), keys);
        (void)end;
        if (ec != std::errc{} || std::find(kPianoKeyboardSizes.begin(), kPianoKeyboardSizes.end(), keys) ==
                                     kPianoKeyboardSizes.end()) return false;
        return set_keyboard_key_count(keys);
    }
    if (actionId == "view.zoom_in" || actionId == "view.zoom_out") {
        remember_camera_position();
        const SettingValue zoomValue = workspace_.settings().value("camera.zoom_sensitivity");
        const float sensitivity = std::get_if<double>(&zoomValue)
            ? static_cast<float>(std::get<double>(zoomValue)) : 1.0F;
        zoom_camera(camera_, (actionId == "view.zoom_in" ? 1.0F : -1.0F) * sensitivity);
        return true;
    }
    if (actionId == "view.previous_camera") {
        if (!previousCamera_) { set_status("No previous camera position"); return true; }
        nextCamera_ = camera_;
        camera_ = *previousCamera_;
        previousCamera_.reset();
        set_status("Previous camera position");
        return true;
    }
    if (actionId == "view.next_camera") {
        if (!nextCamera_) { set_status("No next camera position"); return true; }
        previousCamera_ = camera_;
        camera_ = *nextCamera_;
        nextCamera_.reset();
        set_status("Next camera position");
        return true;
    }
    if (actionId == "view.isolate_selection") return toggle_isolation();
    if (actionId == "view.frame_all") {
        const auto selection = workspace_.selected_objects();
        const auto active = workspace_.selected_object();
        workspace_.clear_selection();
        frame_selection();
        for (EditorObjectId id : selection) workspace_.add_to_selection(id);
        // Re-adding in id order used to make the highest id the active object.
        if (active) workspace_.add_to_selection(*active);
        return true;
    }
    if (actionId == "view.follow_selection") {
        if (!followSelection_ && workspace_.selected_objects().empty()) {
            set_status("Select something to follow first", true);
            return false;
        }
        followSelection_ = !followSelection_;
        followAnchor_.reset();
        if (followSelection_) {
            frame_selection();
            followAnchor_ = selection_pivot();
        }
        refresh_menu_state();
        set_status(followSelection_ ? "Following the selection: the camera moves with it (Shift+F stops)"
                                    : "Stopped following the selection");
        return true;
    }
    if (actionId == "view.drop_to_surface") return drop_selection_to_surface();
    if (actionId == "view.game_view") return toggleSessionSetting("viewport.game_view");
    if (actionId == "view.realtime") return toggleSessionSetting("viewport.realtime");
    if (actionId == "view.immersive") return toggleSessionSetting("viewport.immersive");
    if (actionId == "view.lit") return setSessionSetting("viewport.shading_mode", std::string("lit"));
    if (actionId == "view.unlit") return setSessionSetting("viewport.shading_mode", std::string("unlit"));
    if (actionId == "view.wireframe") return setSessionSetting("viewport.shading_mode", std::string("wireframe"));
    if (actionId == "view.material_ids") return setSessionSetting("viewport.shading_mode", std::string("material_ids"));
    if (actionId == "view.normals") return setSessionSetting("viewport.shading_mode", std::string("normals"));
    if (actionId == "view.voxel_debug") return setSessionSetting("viewport.shading_mode", std::string("voxel_debug"));
    if (actionId == "view.lighting_only") return setSessionSetting("viewport.shading_mode", std::string("lighting_only"));
    if (actionId == "view.overdraw") return setSessionSetting("viewport.shading_mode", std::string("overdraw"));
    if (actionId == "camera.create_from_view") return dispatch_action("create.camera");
    if (actionId == "camera.align_to_view") {
        if (!selectedCameraRig_) { set_status("No camera rig selected", true); return false; }
        camera::CameraRig* rig = cameraDirector_.find_rig(*selectedCameraRig_);
        if (!rig) { set_status("Selected camera rig no longer exists", true); return false; }
        rig->authoredPose = to_camera_pose(camera_, static_cast<float>(std::max(1, layout_.viewport.width)) /
                                                   static_cast<float>(std::max(1, layout_.viewport.height)));
        set_status("Aligned selected camera to viewport");
        return true;
    }
    if (actionId == "camera.toggle_preview") return toggleSessionSetting("camera.preview_selected");
    if (actionId == "camera.toggle_safe_frames") return toggleSessionSetting("viewport.safe_frames");
    if (actionId == "camera.lock") { set_status("Camera rig lock toggled"); return true; }
    if (actionId == "camera.capture_screenshot" || actionId == "camera.capture_selected") {
        set_status("Screenshot capture requested");
        return true;
    }
    if (actionId.starts_with("camera.save_bookmark_") || actionId.starts_with("camera.load_bookmark_") ||
        actionId.starts_with("camera.blend_bookmark_")) {
        const std::size_t underscore = actionId.find_last_of('_');
        if (underscore == std::string_view::npos) return false;
        const int number = std::atoi(std::string(actionId.substr(underscore + 1U)).c_str());
        if (number < 1 || number > 9) return false;
        if (actionId.starts_with("camera.save_bookmark_")) save_camera_bookmark(static_cast<std::size_t>(number - 1));
        else if (!load_camera_bookmark(static_cast<std::size_t>(number - 1))) return false;
        if (actionId.starts_with("camera.blend_bookmark_")) set_status("Blended to camera bookmark " + std::to_string(number));
        return true;
    }
    if (actionId == "play.play") return dispatch_action("physics.play");
    if (actionId == "play.simulate") return dispatch_action("physics.simulate");
    if (actionId == "play.stop") return dispatch_action("physics.stop");
    if (actionId == "play.pause") {
        std::string error;
        const bool success = playSession_.toggle_pause(&error);
        set_status(success ? (playSession_.paused() ? "Simulation paused" : "Simulation resumed") : error, !success);
        return success;
    }
    if (actionId == "play.step") {
        std::string error;
        const bool success = playSession_.single_step(workspace_, &error);
        set_status(success ? "Advanced one fixed simulation tick" : error, !success);
        return success;
    }
    if (actionId == "play.eject") { toggle_play_camera_possession(); return playSession_.active(); }
    if (actionId == "play.keep_changes") return stop_play_session(true);
    if (actionId == "play.release_cursor") { set_status("Mouse capture toggled"); return true; }
    if (actionId == "voxel.brush") { set_active_tool(EditorToolId::AddVoxel); return true; }
    if (actionId == "voxel.erase") { set_active_tool(EditorToolId::RemoveVoxel); return true; }
    if (actionId == "voxel.toggle_operation") {
        set_active_tool(activeTool_ == EditorToolId::RemoveVoxel ? EditorToolId::AddVoxel : EditorToolId::RemoveVoxel);
        return true;
    }
    if (actionId == "voxel.line") { set_active_tool(EditorToolId::Beam); return true; }
    if (actionId == "voxel.volume") { set_active_tool(EditorToolId::Box); return true; }
    if (actionId == "voxel.carve") { set_active_tool(EditorToolId::RemoveVoxel); return true; }
    if (actionId == "voxel.paint_material") { set_active_tool(EditorToolId::PaintMaterial); return true; }
    if (actionId == "voxel.sample_material") {
        if (hoverPick_ && hoverPick_->material != kAirMaterial) {
            set_active_material(hoverPick_->material);
            set_status("Sampled material " + std::to_string(activeMaterial_));
        } else set_status("No voxel material under cursor");
        return true;
    }
    if (actionId == "voxel.brush_shape_cube" || actionId == "voxel.brush_shape_sphere" || actionId == "voxel.brush_shape_toggle") {
        voxelBrushShape_ = actionId == "voxel.brush_shape_cube" ? BrushShape::Cube
            : actionId == "voxel.brush_shape_sphere" ? BrushShape::Sphere
            : (voxelBrushShape_ == BrushShape::Cube ? BrushShape::Sphere : BrushShape::Cube);
        refresh_menu_state();
        set_status(std::string("Voxel brush shape: ") + (voxelBrushShape_ == BrushShape::Sphere ? "sphere" : "cube") +
                   ", radius " + std::to_string(voxelBrushRadius_) + " ([ / ] change it)");
        return true;
    }
    if (actionId == "voxel.brush_increase" || actionId == "voxel.brush_decrease") {
        voxelBrushRadius_ = std::clamp(voxelBrushRadius_ + (actionId == "voxel.brush_increase" ? 1 : -1), 0, 32);
        set_status("Voxel brush radius: " + std::to_string(voxelBrushRadius_) +
                   (voxelBrushShape_ == BrushShape::Sphere ? " (sphere)" : " (cube)"));
        return true;
    }
    if (actionId.starts_with("select.shape_") || actionId.starts_with("select.paint_radius"))
        return dispatch_selection_shape_action(actionId);
    if (actionId.starts_with("select.mask_") || actionId == "edit.select_invert" || actionId == "edit.select_grow" ||
        actionId == "edit.select_shrink" || actionId == "edit.select_connected")
        return dispatch_selection_filter_action(actionId);
    if (actionId == "voxel.fill") return arm_flood_fill();
    if (actionId == "select.matching" || actionId.starts_with("select.similar_")) return dispatch_select_by_property_action(actionId);
    if (actionId == "select.domain_objects") { set_selection_domain(SelectionDomain::Objects); return true; }
    if (actionId == "select.domain_voxels") { set_selection_domain(SelectionDomain::Voxels); return true; }
    if (actionId == "select.domain_toggle") {
        set_selection_domain(selectionDomain_ == SelectionDomain::Objects ? SelectionDomain::Voxels : SelectionDomain::Objects);
        return true;
    }
    if (actionId.starts_with("subdiv.") || actionId=="create.subdivision")
        return dispatch_subdivision_action(actionId=="create.subdivision" ? "subdiv.create_cube" : actionId);
    if (actionId.starts_with("mirror.")) {
        const int axis = actionId.back() == 'x' ? 0 : actionId.back() == 'y' ? 1 : 2;
        if (actionId == "mirror.apply_live") return apply_live_mirror();
        if (actionId.starts_with("mirror.mesh_")) return mirror_mesh(axis, false);
        if (actionId.starts_with("mirror.faces_")) return mirror_mesh(axis, true);
        if (actionId.starts_with("mirror.live_")) return toggle_live_mirror(axis);
    }
    if (actionId.starts_with("mesh.")) return dispatch_mesh_authoring_action(actionId);
    if (actionId.starts_with("polygon.") && actionId != "polygon.import" && actionId != "polygon.import_exact" &&
        actionId != "polygon.create_plane" &&
        actionId != "polygon.lod" && actionId != "polygon.convert_to_voxels" && actionId != "polygon.collision" && actionId != "polygon.streaming")
        return handle_polygon_action(actionId);
    if (selectionDomain_ == SelectionDomain::Polygons && (actionId == "edit.select_all" || actionId == "edit.copy" ||
        actionId == "edit.cut" || actionId == "edit.delete" || actionId == "edit.duplicate")) return handle_polygon_action(actionId);
    if (actionId == "edit.copy" || actionId == "edit.cut") polygonClipboardNewest_=false;
    if (actionId == "edit.paste" && polygonClipboardNewest_ && polygonClipboard_) {
        const auto before=capture_selection_snapshot(); const auto id=workspace_.document().allocate_object_id();
        EditorObject part(id,"Pasted polygon faces"); part.polygon=*polygonClipboard_; part.polygon->objectId=id;
        refresh_editor_polygon(*part.polygon); part.transform=polygonClipboardPose_; part.voxelScale=polygonClipboardScale_;
        part.subdivision=polygonClipboardSubdivision_;
        const auto result=workspace_.commands().execute(workspace_.document(),std::make_unique<AddObjectCommand>(std::move(part),"Paste polygon faces"));
        if (result.success) { if (!viewportSettings_.isolatedObjects.empty()) viewportSettings_.isolatedObjects.insert(id);
            workspace_.select_object(id); selectionDomain_=SelectionDomain::Objects;
            workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot()); recompute_layout(); }
        return result.success;
    }
    if (actionId == "voxel.select_grow") return grow_voxel_selection_cmd();
    if (actionId == "voxel.select_shrink") return shrink_voxel_selection_cmd();
    if (actionId == "voxel.select_invert") return invert_voxel_selection_cmd();
    if (actionId == "voxel.select_all") return select_all_voxels();
    if (actionId == "voxel.select_none") {
        if (voxelSelection_.empty()) { set_status("No voxels are selected", true); return false; }
        clear_voxel_selection();
        set_status("Voxel selection cleared");
        return true;
    }
    if (actionId == "voxel.extract_selection") return extract_selected_voxels();
    if (actionId == "polygon.convert_to_voxels") return begin_geometry_conversion(true);
    if (actionId == "voxel.convert_to_polygon") return begin_geometry_conversion(false);
    if (actionId == "voxel.select_cells" || actionId == "voxel.select_connected") {
        set_selection_domain(SelectionDomain::Voxels); set_active_tool(EditorToolId::Select);
        voxelSelectionBrushEnabled_ = false;
        if (selectionShape_ == SelectionShape::Paint) selectionShape_ = SelectionShape::Box;
        voxelClickSelection_ = actionId == "voxel.select_cells" ? VoxelClickSelection::Cell : VoxelClickSelection::ConnectedPiece;
        refresh_menu_state();
        set_status(voxelClickSelection_ == VoxelClickSelection::Cell ? "Click selects one voxel cell; box modifiers are unchanged"
                                                                  : "Click selects the face-connected piece");
        return true;
    }
    if (actionId == "voxel.selection_brush") {
        const bool enable = !(selectionDomain_ == SelectionDomain::Voxels && activeTool_ == EditorToolId::Select &&
                              voxelSelectionBrushEnabled_);
        set_selection_domain(SelectionDomain::Voxels);
        set_active_tool(EditorToolId::Select);
        voxelSelectionBrushEnabled_ = enable;
        if (enable) selectionShape_ = SelectionShape::Paint;
        else if (selectionShape_ == SelectionShape::Paint) selectionShape_ = SelectionShape::Box;
        voxelBrushSelecting_ = false;
        marquee_ = {};
        refresh_menu_state();
        set_status(enable ? "Selection Brush: drag adds, Ctrl-drag removes; [ / ] changes radius; choose Selection Brush again for boxes"
                          : "Box selection: left-to-right contains, right-to-left crosses; Shift adds, Ctrl removes, Ctrl+Shift intersects");
        return true;
    }
    if (actionId == "voxel.region_extrude_x_positive") return begin_voxel_region_preview(VoxelRegionEdit::Extrude, {0,1});
    if (actionId == "voxel.region_extrude_x_negative") return begin_voxel_region_preview(VoxelRegionEdit::Extrude, {0,-1});
    if (actionId == "voxel.region_extrude_y_positive") return begin_voxel_region_preview(VoxelRegionEdit::Extrude, {1,1});
    if (actionId == "voxel.region_extrude_y_negative") return begin_voxel_region_preview(VoxelRegionEdit::Extrude, {1,-1});
    if (actionId == "voxel.region_extrude_z_positive") return begin_voxel_region_preview(VoxelRegionEdit::Extrude, {2,1});
    if (actionId == "voxel.region_extrude_z_negative") return begin_voxel_region_preview(VoxelRegionEdit::Extrude, {2,-1});
    if (actionId == "voxel.region_rotate_x_positive") return begin_voxel_region_preview(VoxelRegionEdit::RotateQuarterTurn, {0,1});
    if (actionId == "voxel.region_rotate_x_negative") return begin_voxel_region_preview(VoxelRegionEdit::RotateQuarterTurn, {0,-1});
    if (actionId == "voxel.region_rotate_y_positive") return begin_voxel_region_preview(VoxelRegionEdit::RotateQuarterTurn, {1,1});
    if (actionId == "voxel.region_rotate_y_negative") return begin_voxel_region_preview(VoxelRegionEdit::RotateQuarterTurn, {1,-1});
    if (actionId == "voxel.region_rotate_z_positive") return begin_voxel_region_preview(VoxelRegionEdit::RotateQuarterTurn, {2,1});
    if (actionId == "voxel.region_rotate_z_negative") return begin_voxel_region_preview(VoxelRegionEdit::RotateQuarterTurn, {2,-1});
    if (actionId == "voxel.region_scale_2") return begin_voxel_region_preview(VoxelRegionEdit::ScaleInteger, {0,2});
    if (actionId == "voxel.region_scale_3") return begin_voxel_region_preview(VoxelRegionEdit::ScaleInteger, {0,3});
    if (actionId == "voxel.region_scale_4") return begin_voxel_region_preview(VoxelRegionEdit::ScaleInteger, {0,4});

    if (selectionDomain_ == SelectionDomain::Voxels) {
        prune_voxel_selection();
        if (actionId == "edit.select_all") return select_all_voxels();
        // In the Voxel domain these act on voxels only; with none selected they do nothing
        // (rather than falling back to deleting or copying the whole object).
        if (actionId == "edit.delete") return delete_selected_voxels();
        if (actionId == "edit.copy" || actionId == "edit.cut") {
            if (voxelSelection_.empty()) { set_status("No voxels selected (press M to work on objects)", true); return false; }
            return copy_selected_voxels(actionId == "edit.cut");
        }
    }
    if (actionId == "edit.copy" || actionId == "edit.cut") voxelClipboardNewest_ = false;
    if (actionId == "edit.paste" && voxelClipboardNewest_ && voxelClipboard_) return paste_voxels();
    if (actionId == "voxel.commit") { finish_voxel_stroke(); set_status("Voxel operation committed"); return true; }
    if (actionId == "voxel.rebuild_brickmaps") {
        // The native editor draws voxels directly from the document; it keeps no brickmaps.
        set_status(std::string(kRebuildBrickmapsUnavailable), true, 6.0F);
        return false;
    }
    if (actionId == "voxel.rebuild_collision") return rebuild_collision_proxies();
    if (actionId == "render.gabor.enabled") return toggleSessionSetting("render.gabor.enabled");
    if (actionId == "render.gabor.mode_absorption") return setSessionSetting("render.gabor.mode", std::string("absorption"));
    if (actionId == "render.gabor.mode_emission_absorption") return setSessionSetting("render.gabor.mode", std::string("emission_absorption"));
    if (actionId == "render.gabor.mode_scattering") return setSessionSetting("render.gabor.mode", std::string("scattering"));
    if (actionId == "render.gabor.quality_low") return setSessionSetting("render.gabor.quality", std::string("low"));
    if (actionId == "render.gabor.quality_medium") return setSessionSetting("render.gabor.quality", std::string("medium"));
    if (actionId == "render.gabor.quality_high") return setSessionSetting("render.gabor.quality", std::string("high"));
    if (actionId == "render.gabor.quality_cinematic") return setSessionSetting("render.gabor.quality", std::string("cinematic"));
    if (actionId == "render.gabor.continuous_lod") return toggleSessionSetting("render.gabor.continuous_lod");
    if (actionId == "render.gabor.temporal") return toggleSessionSetting("render.gabor.temporal_accumulation");
    if (actionId == "render.gabor.shadows") return toggleSessionSetting("render.gabor.cast_shadows");
    if (actionId == "render.gabor.settings") return openProjectCategory("Rendering");
    if (actionId == "render.voxel_material_policy") return openProjectCategory("Voxel");

    if (actionId == "sprite.pixel_art") {
        spritePixelArtOpen_ = !spritePixelArtOpen_;
        if (spritePixelArtOpen_) {
            spriteRig2DOpen_ = false;
            spriteAnimationGraphOpen_ = false;
            tileWorldEditorOpen_ = false;
            const std::filesystem::path root = projectRoot_.empty() ? std::filesystem::current_path() : projectRoot_;
            spritePixelArtPath_ = root / "assets/sprites/v222_pixel_workspace.dvepixel";
            set_status("Pixel Art Studio | 1 pencil, 2 erase, 3 fill, N frame, L layer, W wrap, Ctrl+S save, Ctrl+P publish", false, 8.0F);
        } else set_status("Pixel Art Studio closed");
        close_top_level_menu(); close_context_menu();
        return true;
    }
    if (actionId == "sprite.rig2d") {
        spriteRig2DOpen_ = !spriteRig2DOpen_;
        if (spriteRig2DOpen_) {
            spritePixelArtOpen_ = false;
            spriteAnimationGraphOpen_ = false;
            tileWorldEditorOpen_ = false;
            const std::filesystem::path root = projectRoot_.empty() ? std::filesystem::current_path() : projectRoot_;
            spriteRig2DPath_ = root / "assets/sprites/v222_character.dvespriterig";
            if (spriteRig2D_.asset().parts.empty()) {
                std::string setupError;
                SpriteRigBoneId body{};
                (void)spriteRig2D_.add_bone("body", 1U, {{0.0F, 8.0F}, 0.0F, {1.0F, 1.0F}}, 12.0F, &body, &setupError);
                SpriteRigPart2D part; part.name = "body"; part.bone = body;
                part.spriteAsset = "assets/sprites/v218_sun_route_cast.dvesprite"; part.clip = "player_idle";
                (void)spriteRig2D_.add_part(part, nullptr, &setupError);
            }
            set_status("Multi-Part Sprite Rig | Space preview, B bone, P part, I IK, V variant, Ctrl+S save", false, 8.0F);
        } else set_status("Multi-Part Sprite Rig closed");
        close_top_level_menu(); close_context_menu();
        return true;
    }
    if (actionId == "render.diagnostics3d") {
        render3DDiagnosticsOpen_ = !render3DDiagnosticsOpen_;
        close_top_level_menu();
        set_status(render3DDiagnosticsOpen_
            ? "3D diagnostics enabled | geometry, pipelines, residency, LOD, skinning, shadows, lights, occlusion, depth complexity"
            : "3D diagnostics hidden", false, 6.0F);
        return true;
    }
    if (actionId == "sprite.diagnostics") {
        spriteDiagnosticsOpen_ = !spriteDiagnosticsOpen_;
        close_top_level_menu();
        set_status(spriteDiagnosticsOpen_
            ? "Sprite diagnostics enabled | counts, residency, sorting, overdraw, snapping, references, budgets"
            : "Sprite diagnostics hidden", false, 6.0F);
        return true;
    }
    if (actionId == "sprite.tile_world") {
        if (tileWorldEditorOpen_) {
            tileWorldEditorOpen_ = false;
            set_status("Tile World Editor closed");
            return true;
        }
        const std::filesystem::path root = projectRoot_.empty()
            ? std::filesystem::current_path() : projectRoot_;
        std::string error;
        if (!tileWorldEditor_.open(root / "assets/tilemaps/v213_original_level.dvetilemap",
                                   root / "assets/tilemaps/v213_original_tiles.dvetileset",
                                   &error)) {
            set_status(error.empty() ? "Could not open the original tile world" : error, true, 7.0F);
            return false;
        }
        tileWorldEditor_.set_preview_camera(Camera2D{{160.0F, 90.0F}, 320.0F, 180.0F, 1.0F});
        tileWorldEditorOpen_ = true;
        tileWorldEditorTicks_ = 0U;
        spriteAnimationGraphOpen_ = false;
        close_top_level_menu();
        close_context_menu();
        set_status("Tile World Editor | 1-7 tools, Ctrl+S save, F5 play test, A/V/C overlays", false, 7.0F);
        return true;
    }
    if (actionId == "sprite.graph") {
        if (spriteAnimationGraphOpen_) {
            spriteAnimationGraphOpen_ = false;
            set_status("Sprite animation graph closed");
            return true;
        }
        const std::filesystem::path root = projectRoot_.empty()
            ? std::filesystem::current_path() : projectRoot_;
        const auto spriteRead = read_dvesprite(root / "assets/sprites/v218_sun_route_cast.dvesprite");
        if (!spriteRead) {
            set_status(spriteRead.error.empty() ? "Could not load graph sprite asset" : spriteRead.error, true, 7.0F);
            return false;
        }
        std::string error;
        if (!spriteAnimationGraph_.open(root / "assets/sprites/v218_sun_route_player.dvesprite_machine",
                                        spriteRead.asset, &error)) {
            set_status(error.empty() ? "Could not open sprite animation graph" : error, true, 7.0F);
            return false;
        }
        spriteAnimationGraph_.set_live_state("Idle");
        spriteAnimationGraphOpen_ = true;
        close_top_level_menu();
        close_context_menu();
        set_status("Sprite animation graph | drag nodes/edges, Ctrl+C/V, B/P/E/M policies, C comment, G group", false, 7.0F);
        return true;
    }
    if (actionId == "sprite.play_level") {
        if (spriteLevelPlaying_) {
            spriteLevelPlaying_ = false;
            spriteLevelClock_ = {};
            spriteLevelPresentation_.reset();
            spriteLevel_.reset();
            spriteLevelInput_ = {};
            set_status("Original sprite level stopped");
            return true;
        }
        const std::filesystem::path root = projectRoot_.empty()
            ? std::filesystem::current_path() : projectRoot_;
        auto level = std::make_unique<gameplay::ChiptuneVerticalSlice>(audioMixer_);
        auto presentation = std::make_unique<gameplay::SpriteVerticalSlicePresentation>();
        std::string error;
        if (!level->initialize(root, {}, &error)) {
            set_status(error.empty() ? "Could not initialize the original sprite level" : error, true, 7.0F);
            return false;
        }
        if (!presentation->initialize(root, {}, &error)) {
            set_status(error.empty() ? "Could not initialize sprite presentation" : error, true, 7.0F);
            return false;
        }
        spriteLevel_ = std::move(level);
        spriteLevelPresentation_ = std::move(presentation);
        spriteLevelInput_ = {};
        spriteLevelClock_ = {};
        spriteLevelPlaying_ = true;
        close_top_level_menu();
        close_context_menu();
        set_status("Original sprite level running | arrows/A-D move, Space jump, X fire, R restart, Esc stop", false, 7.0F);
        return true;
    }
    if (actionId == "sprite.editor") {
        if (assetBrowserState_.selectedId) {
            const EditorAssetRecord* selected = std::as_const(assetDatabase_).find(*assetBrowserState_.selectedId);
            if (selected && (selected->kind == EditorAssetKind::Sprite || selected->kind == EditorAssetKind::Texture)) {
                return dispatch_action("asset.open");
            }
        }
        spriteAuthoringPanel_.toggle();
        set_status(spriteAuthoringPanel_.open()
            ? "Sprite Editor opened; select a texture or .dvesprite asset and choose Open"
            : "Sprite Editor closed");
        return true;
    }
    if (actionId == "sprite.save") {
        std::string error;
        const bool success = spriteAuthoringPanel_.open() && spriteAuthoringPanel_.save(&error);
        set_status(success ? spriteAuthoringPanel_.status()
                           : (error.empty() ? "Open a sprite asset before saving" : error), !success);
        return success;
    }
    if (actionId == "sprite.reimport") {
        std::string error;
        const bool success = spriteAuthoringPanel_.open() &&
            spriteAuthoringPanel_.workspace().reimport_source(true, &error);
        set_status(success ? "Sprite source reimported without losing editor selection"
                           : (error.empty() ? "Open a sprite asset before reimporting" : error), !success);
        return success;
    }
    if (actionId == "sprite.slice_grid") {
        std::string error;
        const bool success = spriteAuthoringPanel_.open() &&
            spriteAuthoringPanel_.workspace().slice_grid(spriteAuthoringPanel_.grid_settings(), &error);
        set_status(success ? "Sprite grid sliced"
                           : (error.empty() ? "Open a sprite texture before slicing" : error), !success);
        return success;
    }
    if (actionId == "sprite.repack") {
        SpriteAtlasPackResult packed;
        std::string error;
        const bool success = spriteAuthoringPanel_.open() &&
            spriteAuthoringPanel_.workspace().repack_atlas({}, packed, &error);
        set_status(success ? "Sprite atlas repacked to " + std::to_string(packed.width) + "x" +
                            std::to_string(packed.height)
                           : (error.empty() ? "Open a sprite asset before packing" : error), !success);
        return success;
    }

    if (actionId == "material.open_layers") return dispatch_action("material.layers");
    if (actionId == "material.open_globals") return dispatch_action("material.globals");
    // Commands that exist in the shortcut set but have no native-editor implementation yet: say
    // so (and what to use instead) instead of reporting success.
    if (actionId == "material.assign" || actionId == "material.apply") {
        set_status("Not in this editor yet: use the Paint tool (5) or Voxel > Flood Fill to apply the active material", true, 6.0F);
        return false;
    }
    if (actionId == "material.link" || actionId == "material.create_instance" || actionId == "material.create_master" ||
        actionId == "material.preview" || actionId == "material.pin_preview") {
        set_status("Not in this editor yet: material instances and previews; edit materials under Materials > Material Library", true, 6.0F);
        return false;
    }
    // polygon.* / audio.* / sequencer.* commands without their own handler are refused at the
    // end of dispatch (after polygon.lod, polygon.import, audio.settings, audio.mixer, ... had
    // their chance; a catch-all here used to shadow those menu items).

    if (actionId == "edit.preferences") { open_settings(SettingScope::User, "General"); return true; }
    if (actionId == "edit.project_settings" || actionId == "window.settings") {
        open_settings(SettingScope::Project, "General"); return true;
    }
    if (actionId == "camera.settings") return openProjectCategory("Camera");
    if (actionId == "camera.inspector" || actionId == "window.cinematic_camera") {
        cinematicCameraPanel_.toggle();
        set_status(cinematicCameraPanel_.is_open() ? "Cinematic Camera Inspector opened"
                                                   : "Cinematic Camera Inspector closed");
        refresh_menu_state();
        return true;
    }
    if (actionId == "camera.sequencer" || actionId == "window.camera_sequencer") {
        cinematicCameraPanel_.toggle_sequencer();
        if (cinematicCameraPanel_.sequencer_open()) cinematicCameraPanel_.open();
        set_status(cinematicCameraPanel_.sequencer_open() ? "Camera Sequencer opened"
                                                          : "Camera Sequencer closed");
        refresh_menu_state();
        return true;
    }
    if (actionId == "window.camera_lut_browser") {
        cinematicCameraPanel_.open();
        cinematicCameraPanel_.set_section(CinematicCameraSection::ColorGrade);
        cinematicCameraPanel_.set_status("LUT browser and color pipeline");
        set_status("Cinematic Camera Inspector opened to LUT and color grading");
        refresh_menu_state();
        return true;
    }
    if (actionId == "window.camera_diagnostics") {
        cinematicCameraPanel_.open();
        cinematicCameraPanel_.set_status("Camera diagnostics: overlays, profile ownership, and sequence state");
        set_status("Camera diagnostics opened");
        refresh_menu_state();
        return true;
    }
    if (actionId == "camera.scope_instance" || actionId == "camera.scope_shot" ||
        actionId == "camera.scope_project" || actionId == "camera.scope_preview") {
        CinematicCameraScope scope = CinematicCameraScope::CameraInstance;
        if (actionId == "camera.scope_shot") scope = CinematicCameraScope::ShotOverride;
        else if (actionId == "camera.scope_project") scope = CinematicCameraScope::ProjectDefault;
        else if (actionId == "camera.scope_preview") scope = CinematicCameraScope::ViewportPreview;
        cinematicCameraPanel_.set_scope(scope);
        cinematicCameraPanel_.open();
        cinematicCameraPanel_.set_status(std::string(EditorCinematicCameraPanel::scope_name(scope)) + " camera edits");
        set_status(std::string("Editing ") + std::string(EditorCinematicCameraPanel::scope_name(scope)) + " camera settings");
        refresh_menu_state();
        return true;
    }
    if (actionId.starts_with("camera.preset_")) {
        std::string error;
        const auto preset = cinematic_preset_from_id(actionId);
        const bool applied = cinematicCameraPanel_.apply_preset(preset, selectedCameraRig_, &error);
        if (applied && cinematicCameraPanel_.scope() == CinematicCameraScope::ProjectDefault) {
            (void)workspace_.settings().set(SettingScope::Project,
                "camera.default_cinematic_preset", std::string(cinematic_preset_setting_id(preset)), &error);
        }
        if (applied && cinematicCameraPanel_.scope() == CinematicCameraScope::ShotOverride) {
            const camera::CameraRig* rig = selectedCameraRig_ ? cameraDirector_.find_rig(*selectedCameraRig_) : nullptr;
            const camera::CameraPose pose = rig ? rig->authoredPose
                : to_camera_pose(camera_, static_cast<float>(std::max(1, layout_.viewport.width)) /
                                           static_cast<float>(std::max(1, layout_.viewport.height)));
            if (!cinematicCameraPanel_.keyframe_current(pose, selectedCameraRig_, &error)) {
                set_status(error, true);
                return false;
            }
        }
        if (applied) {
            cinematicCameraPanel_.open();
            set_status(std::string("Applied cinematic preset: ") +
                       std::string(EditorCinematicCameraPanel::preset_name(preset)));
        } else set_status(error, true);
        refresh_menu_state();
        return applied;
    }
    if (actionId.starts_with("camera.filmback_")) {
        camera::CameraRig* rig = selectedCameraRig_ ? cameraDirector_.find_rig(*selectedCameraRig_) : nullptr;
        std::string error;
        const auto preset = filmback_preset_from_id(actionId);
        const bool applied = cinematicCameraPanel_.apply_filmback(preset, camera_, rig, &error);
        if (applied && cinematicCameraPanel_.scope() == CinematicCameraScope::ProjectDefault) {
            (void)workspace_.settings().set(SettingScope::Project,
                "camera.sensor_preset", std::string(filmback_setting_id(preset)), &error);
        }
        if (applied && cinematicCameraPanel_.scope() == CinematicCameraScope::ShotOverride) {
            const camera::CameraPose pose = rig ? rig->authoredPose
                : to_camera_pose(camera_, static_cast<float>(std::max(1, layout_.viewport.width)) /
                                           static_cast<float>(std::max(1, layout_.viewport.height)));
            if (!cinematicCameraPanel_.keyframe_current(pose, selectedCameraRig_, &error)) {
                set_status(error, true);
                return false;
            }
        }
        set_status(applied ? std::string("Applied filmback: ") +
                                 std::string(EditorCinematicCameraPanel::filmback_name(preset))
                           : error, !applied);
        refresh_menu_state();
        return applied;
    }
    if (actionId == "camera.copy_profile") {
        cinematicCameraPanel_.copy_profile(selectedCameraRig_);
        set_status("Copied cinematic camera profile");
        refresh_menu_state();
        return true;
    }
    if (actionId == "camera.paste_profile") {
        std::string error;
        const bool pasted = cinematicCameraPanel_.paste_profile(selectedCameraRig_, &error);
        set_status(pasted ? "Pasted cinematic camera profile" : error, !pasted);
        refresh_menu_state();
        return pasted;
    }
    if (actionId == "camera.clear_effects") {
        std::string error;
        const bool cleared = cinematicCameraPanel_.clear_effects(selectedCameraRig_, &error);
        set_status(cleared ? "Cleared cinematic camera effects" : error, !cleared);
        refresh_menu_state();
        return cleared;
    }
    if (actionId == "camera.keyframe_profile") {
        const camera::CameraRig* rig = selectedCameraRig_ ? cameraDirector_.find_rig(*selectedCameraRig_) : nullptr;
        const camera::CameraPose pose = rig ? rig->authoredPose
            : to_camera_pose(camera_, static_cast<float>(std::max(1, layout_.viewport.width)) /
                                       static_cast<float>(std::max(1, layout_.viewport.height)));
        std::string error;
        const bool keyed = cinematicCameraPanel_.keyframe_current(pose, selectedCameraRig_, &error);
        set_status(keyed ? "Keyframed complete cinematic profile" : error, !keyed);
        refresh_menu_state();
        return keyed;
    }
    if (actionId == "camera.pilot_selected") {
        if (!selectedCameraRig_) { set_status("No camera rig selected", true); return false; }
        const camera::CameraRig* rig = cameraDirector_.find_rig(*selectedCameraRig_);
        if (!rig) { set_status("Selected camera rig no longer exists", true); return false; }
        remember_camera_position();
        apply_camera_pose(camera_, rig->authoredPose);
        set_camera_mode(camera::CameraRigMode::Cinematic);
        set_status("Piloting selected camera rig");
        return true;
    }
    if (actionId == "camera.lock_viewport") return toggleSessionSetting("camera.lock_viewport_to_selected");
    if (actionId == "camera.preview_effects") return toggleSessionSetting("camera.preview_cinematic_effects");
    if (actionId == "camera.overlay_focus") {
        cinematicCameraPanel_.overlays().focusPlanes = !cinematicCameraPanel_.overlays().focusPlanes;
    } else if (actionId == "camera.overlay_split_diopter") {
        cinematicCameraPanel_.overlays().splitDiopter = !cinematicCameraPanel_.overlays().splitDiopter;
    } else if (actionId == "camera.overlay_safe_frames") {
        cinematicCameraPanel_.overlays().safeFrames = !cinematicCameraPanel_.overlays().safeFrames;
    } else if (actionId == "camera.overlay_aspect_mattes") {
        cinematicCameraPanel_.overlays().aspectMattes = !cinematicCameraPanel_.overlays().aspectMattes;
    } else if (actionId == "camera.overlay_motion_vectors") {
        cinematicCameraPanel_.overlays().motionVectors = !cinematicCameraPanel_.overlays().motionVectors;
    } else if (actionId == "camera.overlay_exposure") {
        cinematicCameraPanel_.overlays().exposurePreview = !cinematicCameraPanel_.overlays().exposurePreview;
    } else if (actionId == "camera.compare_graded") {
        cinematicCameraPanel_.overlays().compareUngraded = !cinematicCameraPanel_.overlays().compareUngraded;
    } else if (actionId == "camera.reset_preview") {
        const CinematicCameraScope oldScope = cinematicCameraPanel_.scope();
        cinematicCameraPanel_.set_scope(CinematicCameraScope::ViewportPreview);
        std::string error;
        const bool cleared = cinematicCameraPanel_.clear_effects(selectedCameraRig_, &error);
        cinematicCameraPanel_.overlays() = {};
        cinematicCameraPanel_.set_scope(oldScope);
        set_status(cleared ? "Reset camera preview overrides" : error, !cleared);
        refresh_menu_state();
        return cleared;
    } else {
        // Continue to non-camera dispatch below.
        goto camera_menu_dispatch_complete;
    }
    set_status("Camera overlay updated");
    refresh_menu_state();
    return true;
camera_menu_dispatch_complete:
    if (actionId == "voxel.settings") return openProjectCategory("Voxel");
    if (actionId == "polygon.lod" || actionId == "polygon.collision" || actionId == "polygon.streaming")
        return openProjectCategory("Polygon");
    if (actionId == "material.library" || actionId == "material.globals" ||
        actionId == "material.layers" || actionId == "material.rendering") return openProjectCategory("Materials");
    if (actionId == "audio.settings") return openProjectCategory("Audio");
    if (actionId == "physics.settings") return openProjectCategory("Physics");
    if (actionId == "build.settings") return openProjectCategory("Build");

    if (actionId == "camera.mode_free") { set_camera_mode(camera::CameraRigMode::FreeFly); return true; }
    if (actionId == "camera.mode_orbit") { set_camera_mode(camera::CameraRigMode::Orbit); return true; }
    if (actionId == "camera.mode_follow") { set_camera_mode(camera::CameraRigMode::Follow); return true; }
    if (actionId == "camera.mode_third_person") { set_camera_mode(camera::CameraRigMode::ThirdPerson); return true; }
    if (actionId == "camera.mode_first_person") { set_camera_mode(camera::CameraRigMode::FirstPerson); return true; }
    if (actionId == "camera.mode_cinematic") { set_camera_mode(camera::CameraRigMode::Cinematic); return true; }
    if (actionId == "camera.projection_perspective") return setSessionSetting("camera.projection", std::string("perspective"));
    if (actionId == "camera.projection_orthographic") return setSessionSetting("camera.projection", std::string("orthographic"));
    if (actionId == "camera.physical_lens") return toggleSessionSetting("camera.physical_lens");
    if (actionId == "camera.collision") return toggleSessionSetting("camera.collision");
    if (actionId == "camera.preview_selected" || actionId == "window.camera_preview")
        return toggleSessionSetting("camera.preview_selected");
    if (actionId == "camera.show_frustum") return toggleSessionSetting("camera.show_frustum");
    if (actionId == "camera.preview_shakes") return toggleSessionSetting("camera.allow_shakes_in_editor");
    if (actionId == "camera.save_bookmark_1") { save_camera_bookmark(0); return true; }
    if (actionId == "camera.load_bookmark_1") return load_camera_bookmark(0);
    if (actionId == "camera.save_bookmark_2") { save_camera_bookmark(1); return true; }
    if (actionId == "camera.load_bookmark_2") return load_camera_bookmark(1);
    if (actionId == "camera.reset") {
        camera_ = EditorCamera{};
        set_camera_mode(camera::CameraRigMode::Orbit);
        apply_settings_to_runtime();
        frame_selection();
        set_status("Camera reset");
        return true;
    }

    if (actionId == "view.bounds") return toggleSessionSetting("viewport.bounds");
    if (actionId == "view.xray") return toggleSessionSetting("viewport.xray_selection");
    if (actionId == "view.statistics") return toggleSessionSetting("viewport.statistics");
    if (actionId == "view.safe_frames") return toggleSessionSetting("viewport.safe_frames");

    if (actionId == "audio.mixer" || actionId == "audio.workstation") {
        audioPanel_.toggle();
        set_status(audioPanel_.open() ? "Audio workspace opened" : "Audio workspace closed");
        return true;
    }
    if (actionId == "audio.tracker") {
        chiptunePanel_.toggle(audioMixer_);
        set_status(chiptunePanel_.open() ? "Chiptune Tracker opened" : "Chiptune Tracker closed");
        return true;
    }
    if (actionId == "audio.synth") {
        synthPanel_.toggle(audioMixer_.synthesizer());
        set_status(synthPanel_.open() ? "Synthesizer opened" : "Synthesizer closed");
        return true;
    }
    if (actionId == "audio.events") {
        audioEventPanel_.toggle();
        set_status(audioEventPanel_.open() ? "Interactive Audio opened" : "Interactive Audio closed");
        return true;
    }
    if (actionId == "create.camera") {
        set_camera_mode(camera::CameraRigMode::Cinematic);
        return create_camera_rig_from_view() != 0;
    }
    if (actionId == "polygon.import") {
        const auto* asset=assetBrowserState_.selectedId?
            std::as_const(assetDatabase_).find(*assetBrowserState_.selectedId):nullptr;
        if(!asset){set_status("Select an OBJ, glTF, GLB or DMESH model in the Assets panel first.",true);return false;}
        return import_polygon_model(asset->relativePath);
    }
    if (actionId == "polygon.import_exact") {
        const auto* asset=assetBrowserState_.selectedId?
            std::as_const(assetDatabase_).find(*assetBrowserState_.selectedId):nullptr;
        if(!asset){set_status("Select a STEP, BREP or OpenUSD B-rep (.usda) file in the Assets panel first.",true);return false;}
        return import_exact_cad_model(asset->relativePath);
    }
    if (actionId == "create.polygon" || actionId == "polygon.create_plane") {
        if (workspace_.mode() != EditorMode::Edit || playSession_.active()) {
            set_status(std::string(kCreatePolygonBlocked), true);
            return false;
        }
        close_previews_for_voxel_edit("polygon mesh created");
        const bool plane = actionId == "polygon.create_plane";
        auto& document = workspace_.document();
        const auto id = document.allocate_object_id();
        const auto before = capture_selection_snapshot();
        EditorObject object(id, plane ? "Polygon Plane" : "Polygon Cube");
        object.voxelSizeMeters = 1.0F;
        object.polygon = plane ? make_editor_polygon_plane(id) : make_editor_polygon_cube(id);
        object.transform.position = camera_.target;
        object.flags.anchored = plane;
        const auto result = workspace_.commands().execute(
            document, std::make_unique<AddObjectCommand>(std::move(object), plane ? "Create polygon plane" : "Create polygon cube"));
        if (!result.success) { set_status(result.message, true); return false; }
        // A new object stays visible while the view is isolated (as Import and Paste do).
        if (!viewportSettings_.isolatedObjects.empty()) viewportSettings_.isolatedObjects.insert(id);
        workspace_.select_object(id);
        selectionDomain_ = SelectionDomain::Objects;
        workspace_.commands().set_last_selection_transition(before, capture_selection_snapshot());
        frame_selection();
        refresh_menu_state();
        set_status(plane ? "Created a polygon plane" : "Created a polygon cube");
        return true;
    }
    if (actionId == "build.validate") {
        (void)validate_project();
        return true;
    }

    if (actionId == "edit.undo") {
        const auto* selection = workspace_.commands().undo_selection();
        const CommandResult result = workspace_.commands().undo(workspace_.document());
        if (result.success) {
            if (selection) restore_selection_snapshot(*selection);
            else { workspace_.prune_selection(); prune_voxel_selection(); }
            recompute_layout();
            refresh_menu_state();
        }
        set_status(result.success ? "Undo" : result.message, !result.success);
        return result.success;
    }
    if (actionId == "edit.redo") {
        const auto* selection = workspace_.commands().redo_selection();
        const CommandResult result = workspace_.commands().redo(workspace_.document());
        if (result.success) {
            if (selection) restore_selection_snapshot(*selection);
            else { workspace_.prune_selection(); prune_voxel_selection(); }
            recompute_layout();
            refresh_menu_state();
        }
        set_status(result.success ? "Redo" : result.message, !result.success);
        return result.success;
    }
    if (actionId == "edit.select_all") {
        workspace_.clear_selection();
        for (const auto& [id, object] : workspace_.document().objects()) {
            (void)object;
            workspace_.add_to_selection(id);
        }
        set_status("Selected " + std::to_string(workspace_.selection_count()) + " objects");
        return true;
    }
    if (actionId == "transform.translate") { set_active_tool(EditorToolId::Translate); return true; }
    if (actionId == "transform.rotate") { set_active_tool(EditorToolId::Rotate); return true; }
    if (actionId == "transform.space") {
        transformSpace_ = transformSpace_ == EditorTransformSpace::World ? EditorTransformSpace::Local
                                                                         : EditorTransformSpace::World;
        set_status(transformSpace_ == EditorTransformSpace::World ? "World transform axes" : "Local transform axes");
        return true;
    }
    if (actionId == "transform.pivot_bounds" || actionId == "transform.pivot_active" ||
        actionId == "transform.pivot_origin" || actionId == "transform.pivot_cursor") {
        if (actionId == "transform.pivot_cursor") {
            if (!hoverPick_) { set_status("Point at geometry to set a custom pivot", true); return false; }
            customPivot_ = hoverPick_->worldPosition;
            pivotMode_ = PivotMode::Custom;
        } else pivotMode_ = actionId == "transform.pivot_active" ? PivotMode::ActiveObject
            : actionId == "transform.pivot_origin" ? PivotMode::WorldOrigin : PivotMode::BoundsCenter;
        set_status("Transform pivot changed");
        return true;
    }
    if (actionId == "transform.align_surface") {
        alignToSurfaceNormal_ = !alignToSurfaceNormal_;
        refresh_menu_state();
        set_status(alignToSurfaceNormal_ ? "Surface normal alignment enabled" : "Surface normal alignment disabled");
        return true;
    }
    if (actionId == "transform.surface_offset_more" || actionId == "transform.surface_offset_less") {
        placementOffsetMeters_ = std::clamp(placementOffsetMeters_ +
            (actionId == "transform.surface_offset_more" ? 0.05F : -0.05F), -10.0F, 10.0F);
        set_status("Surface offset: " + std::to_string(placementOffsetMeters_) + " m");
        return true;
    }
    if (actionId == "transform.place_surface" || actionId == "transform.place_voxel_corner" ||
        actionId == "transform.place_voxel_edge" || actionId == "transform.place_voxel_face" ||
        actionId == "transform.place_bounds_vertex" || actionId == "transform.place_collision_vertex") {
        if (workspace_.selected_objects().empty()) { set_status("Select an object to place", true); return false; }
        if (placementMode_ != PlacementTarget::NoTarget) finish_placement(true);
        for (EditorObjectId id : workspace_.selected_objects()) {
            const EditorObject* object = workspace_.document().find_object(id);
            if (!object || object->flags.locked) {
                set_status("Unlock the selected objects before placement", true); return false;
            }
        }
        placementMode_ = actionId == "transform.place_surface" ? PlacementTarget::Surface
            : actionId == "transform.place_voxel_corner" ? PlacementTarget::VoxelCorner
            : actionId == "transform.place_voxel_edge" ? PlacementTarget::VoxelEdge
            : actionId == "transform.place_voxel_face" ? PlacementTarget::VoxelFace
            : actionId == "transform.place_collision_vertex" ? PlacementTarget::CollisionBoxVertex
            : PlacementTarget::BoundsVertex;
        placementChanges_ = selection_transform_snapshot();
        placementPivot_ = selection_pivot();
        set_status("Move over a target and click to place; Escape cancels");
        return true;
    }
    if (actionId == "transform.scale_double") {
        return rescale_primary_voxel_object({2.0F, 2.0F, 2.0F}).success;
    }
    if (actionId == "transform.scale_half") {
        return rescale_primary_voxel_object({0.5F, 0.5F, 0.5F}).success;
    }
    if (actionId == "transform.scale_voxel_size_up") return scale_selection_voxel_size(2.0F).success;
    if (actionId == "transform.scale_voxel_size_down") return scale_selection_voxel_size(0.5F).success;
    if (actionId == "hierarchy.reveal_selection") return reveal_selection_in_hierarchy();
    if (actionId == "hierarchy.expand_all" || actionId == "hierarchy.collapse_all") {
        if (!hierarchyFilter_.empty()) {
            set_status("Clear the hierarchy filter before collapsing or expanding rows", true);
            return false;
        }
        collapsedHierarchy_.clear();
        if (actionId == "hierarchy.collapse_all") {
            for (const auto& [id, object] : workspace_.document().objects()) {
                (void)id;
                if (object.parent) collapsedHierarchy_.insert(*object.parent);
            }
        }
        hierarchyDrag_ = {};
        recompute_layout();
        return true;
    }
    if (actionId == "view.frame") { frame_selection(); return true; }
    if (actionId == "view.grid") return toggleSessionSetting("viewport.grid");
    if (actionId == "view.collision") return toggleSessionSetting("viewport.collision");
    if (actionId == "view.anchors") return toggleSessionSetting("viewport.anchors");
    if (actionId == "voxel.add") { set_active_tool(EditorToolId::AddVoxel); return true; }
    if (actionId == "voxel.remove") { set_active_tool(EditorToolId::RemoveVoxel); return true; }
    if (actionId == "voxel.paint") { set_active_tool(EditorToolId::PaintMaterial); return true; }
    if (actionId == "voxel.box") { set_active_tool(EditorToolId::Box); return true; }
    if (actionId == "voxel.beam") { set_active_tool(EditorToolId::Beam); return true; }
    if (actionId == "voxel.anchor") { set_active_tool(EditorToolId::Anchor); return true; }
    if (actionId == "physics.simulate") return start_play_session(EditorMode::Simulate);
    if (actionId == "physics.play") return start_play_session(EditorMode::Play);
    if (actionId == "physics.stop") return stop_play_session(false);
    if (actionId == "help.command_palette") {
        commandPaletteOpen_ = true;
        commandPaletteQuery_.clear();
        commandPaletteSelection_ = 0U;
        close_top_level_menu();
        set_status("Type to search commands; Enter runs the first result", false, 6.0F);
        return true;
    }
    if (actionId == "help.shortcuts") {
        set_status("1 Select  2 Move  3 Add  4 Remove  5 Paint  6 Box  7 Beam  8 Anchor  9 Rotate; Shift-click multi-select", false, 8.0F);
        return true;
    }
    if (actionId == "file.save") {
        if (std::get<bool>(workspace_.settings().value("diagnostics.validation_on_save"))) {
            std::string validationError;
            if (!workspace_.document().validate(&validationError)) {
                set_status("Scene validation failed: " + validationError, true);
                return false;
            }
        }
#ifdef DVE_HAVE_LUA
        {
            // Compile-check the startup script without executing it.
            // scripting.strict_errors decides whether a broken script
            // blocks the save (Block) or saves with the error kept visible
            // in the editor log (Warn); the error is reported either way.
            const auto scriptPath = project_root() / "scripts" / "main.lua";
            std::ifstream scriptStream(scriptPath, std::ios::binary);
            if (scriptStream) {
                const std::string scriptCode{std::istreambuf_iterator<char>(scriptStream),
                                             std::istreambuf_iterator<char>()};
                std::string scriptError;
                if (!check_lua_syntax(scriptCode, &scriptError)) {
                    if (script_error_policy(workspace_.settings()) == ScriptErrorPolicy::Block) {
                        set_status("Script validation failed: " + scriptError, true);
                        return false;
                    }
                    workspace_.log().add(EditorLogLevel::Error,
                        "Startup script compile error (save allowed: strict script errors "
                        "are off): " + scriptError);
                }
            }
        }
#endif
        if (workspace_.document().path().empty()) return open_file_picker(FilePickerKind::SaveSceneAs);  // first save names it
        const auto result = workspace_.document().save_transactional(workspace_.document().path());
        set_status(result.success ? "Scene saved" : result.error, !result.success);
        if (result.success) {
            // The scene file now holds everything the recovery copy did.
            std::error_code ignored;
            std::filesystem::remove_all(autosave_directory(), ignored);
            autosaveElapsedSeconds_ = 0.0;
            ++manualSaveCount_;
        }
        return result.success;
    }
    if (actionId == "file.export_print_stl") {
#ifdef DVE_HAVE_MANIFOLD
        if (workspace_.selection_count() != 1U) {
            set_status("Select exactly one voxel object to export for printing", true);
            return false;
        }
        const EditorObjectId id = *workspace_.selected_objects().begin();
        EditorObject* object = workspace_.document().find_object(id);
        if (object == nullptr || object->voxels == nullptr) {
            set_status("Selected object has no voxel data to export", true);
            return false;
        }
        PrintExportOptions options;
        options.voxelSizeMeters = static_cast<double>(object->voxelSizeMeters);
        options.voxelScale = object->voxelScale;
        std::filesystem::path dir = project_root() / "exports";
        std::error_code dirError;
        std::filesystem::create_directories(dir, dirError);
        std::string name = object->name.empty() ? "model" : object->name;
        for (char& c : name) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') c = '_';
        }
        const std::filesystem::path path = dir / (name + ".stl");
        const PrintExportResult result = export_print_stl(*object->voxels, path, options);
        if (!result.success) {
            set_status(std::string("Print export failed: ") + result.error, true);
            return false;
        }
        set_status("Exported " + std::to_string(result.triangleCount) + " triangles to " + path.string(),
                   false, 8.0F);
        return true;
#else
        set_status("Print export was not built into this editor (manifold3d missing)", true);
        return false;
#endif
    }
    if (actionId == "file.new_project") {
        if (workspace_.document().dirty() && workspace_.preferences().confirmDestructiveActions) {
            pendingDestructiveAction_ = PendingDestructiveAction::NewProject;
            set_status(std::string(pending_confirmation_message()), false, 0.0F);
        } else {
            create_new_project_now();
            set_status("New project");
        }
        return true;
    }
    if (actionId == "file.open_scene") return open_file_picker(FilePickerKind::OpenScene);
    if (actionId == "file.open_project") return open_file_picker(FilePickerKind::OpenProject);
    if (actionId == "file.save_as") return open_file_picker(FilePickerKind::SaveSceneAs);
    if (actionId == "file.import") return import_selected_model();
    if (actionId == "file.reimport") return reimport_selected_model();
    if (actionId == "file.new_scene") {
        if (workspace_.document().dirty() && workspace_.preferences().confirmDestructiveActions) {
            pendingDestructiveAction_ = PendingDestructiveAction::NewScene;
            set_status(std::string(pending_confirmation_message()), false, 0.0F);
        } else {
            create_new_scene_now();
            set_status("New scene");
        }
        return true;
    }
    if (actionId == "file.exit") {
        request_quit();
        set_status(pending_destructive_confirmation() ? std::string(pending_confirmation_message()) : "Exiting",
                   false, pending_destructive_confirmation() ? 0.0F : 3.0F);
        return true;
    }
    if (actionId == "edit.delete") {
        if (workspace_.selection_count() == 0) {
            set_status("Nothing selected to delete", true);
            return false;
        }
        std::vector<EditorObjectId> roots(workspace_.selected_objects().begin(), workspace_.selected_objects().end());
        const std::vector<EditorObjectId> closure = collect_editor_object_subtree_ids(workspace_.document(), roots);
        const std::size_t count = closure.size();
        const CommandResult result = workspace_.commands().execute(
            workspace_.document(),
            std::make_unique<RemoveObjectsCommand>(std::move(roots), count == 1 ? "Delete object" : "Delete objects"));
        if (result.success) {
            workspace_.clear_selection();
            recompute_layout();
            set_status(count == 1 ? "Deleted 1 object" : "Deleted " + std::to_string(count) + " objects");
        } else {
            set_status(result.message, true);
        }
        return result.success;
    }
    if (actionId == "edit.copy" || actionId == "edit.cut") {
        if (workspace_.selection_count() == 0) {
            set_status("Nothing selected to copy", true);
            return false;
        }
        std::vector<EditorObjectId> roots(workspace_.selected_objects().begin(), workspace_.selected_objects().end());
        const std::vector<EditorObjectId> closure = collect_editor_object_subtree_ids(workspace_.document(), roots);
        clipboard_.clear();
        clipboard_.reserve(closure.size());
        for (const EditorObjectId id : closure) {
            const EditorObject* object = workspace_.document().find_object(id);
            if (object) clipboard_.push_back(clone_editor_object(*object));
        }
        const std::size_t copied = clipboard_.size();
        if (actionId == "edit.cut") {
            const CommandResult result = workspace_.commands().execute(
                workspace_.document(), std::make_unique<RemoveObjectsCommand>(std::move(roots), "Cut objects"));
            if (!result.success) {
                clipboard_.clear();
                set_status(result.message, true);
                return false;
            }
            workspace_.clear_selection();
            recompute_layout();
        }
        set_status((actionId == "edit.cut" ? "Cut " : "Copied ") + std::to_string(copied) +
                   (copied == 1 ? " object" : " objects"));
        return true;
    }
    if (actionId == "edit.paste") {
        if (clipboard_.empty()) {
            set_status("Clipboard is empty", true);
            return false;
        }
        constexpr Float3 kPasteOffset{0.5F, 0.0F, 0.5F};
        std::map<EditorObjectId, EditorObjectId> idMap;
        for (const EditorObject& source : clipboard_)
            idMap.emplace(source.id, workspace_.document().allocate_object_id());

        auto compound = std::make_unique<CompoundCommand>(
            clipboard_.size() == 1 ? "Paste object" : "Paste objects");
        std::vector<EditorObjectId> pastedRootIds;
        for (const EditorObject& source : clipboard_) {
            EditorObject pasted = clone_editor_object(source);
            pasted.id = idMap.at(source.id);
            if (source.parent && idMap.contains(*source.parent)) {
                pasted.parent = idMap.at(*source.parent);
            } else if (source.parent && workspace_.document().find_object(*source.parent)) {
                pasted.parent = source.parent;
            } else {
                pasted.parent.reset();
            }
            pasted.transform.position = add(pasted.transform.position, kPasteOffset);
            if (!source.parent || !idMap.contains(*source.parent)) pastedRootIds.push_back(pasted.id);
            compound->add(std::make_unique<AddObjectCommand>(std::move(pasted), "Paste object"));
        }
        const CommandResult result = workspace_.commands().execute(workspace_.document(), std::move(compound));
        if (!result.success) {
            set_status(result.message, true);
            return false;
        }
        workspace_.clear_selection();
        for (const EditorObjectId id : pastedRootIds) workspace_.add_to_selection(id);
        recompute_layout();
        set_status("Pasted " + std::to_string(clipboard_.size()) +
                   (clipboard_.size() == 1 ? " object" : " objects"));
        return true;
    }
    if (actionId == "create.text3d") return create_text3d({});
    if (actionId == "create.gabor_empty") return create_gabor_volume({});
    if (actionId == "create.gabor_import") {
        const auto path = find_default_gabor_asset();
        if (path.empty()) {
            set_status("No .dgabor, .ply, or Gabor pyramid found under assets/volumes", true, 6.0F);
            return false;
        }
        return create_gabor_volume(path);
    }
    if (actionId == "window.gabor_inspector") return openProjectCategory("Rendering");
    if (actionId == "text3d.edit_selected") return edit_selected_text3d();
    if (actionId == "text3d.commit") return commit_text3d_authoring();
    if (actionId == "create.scatter") return begin_scatter();
    if (actionId == "scatter.brush") { set_active_tool(EditorToolId::ScatterBrush); return true; }
    if (actionId == "scatter.set_sources") return set_scatter_sources();
    if (actionId == "scatter.clear_sources") {
        if (!scatter_sources_pinned()) return false;
        clear_scatter_sources();
        return true;
    }
    if (actionId == "scatter.commit") return commit_scatter();
    if (actionId == "scatter.cancel") { if (!scatterActive_) return false; cancel_scatter(); return true; }
    if (actionId == "voxel.join") return begin_voxel_join();
    if (actionId == "voxel.join_commit") return commit_voxel_join().success;
    if (actionId == "voxel.join_cancel") {
        if (!voxelJoin_.active()) return false;
        cancel_voxel_join();
        return true;
    }
    if (actionId == "voxel.separate_islands") return separate_voxel_islands();
    if (actionId == "voxel.slice") return begin_voxel_slice();
    if (actionId == "voxel.slice_commit") return commit_voxel_slice().success;
    if (actionId == "voxel.slice_cancel") { cancel_voxel_slice(); return true; }
    if (actionId == "voxel.boolean_union") return begin_voxel_boolean(VoxelBooleanOperation::Union);
    if (actionId == "voxel.boolean_difference") return begin_voxel_boolean(VoxelBooleanOperation::Difference);
    if (actionId == "voxel.boolean_intersection") return begin_voxel_boolean(VoxelBooleanOperation::Intersection);
    if (actionId == "voxel.boolean_commit") return commit_voxel_boolean().success;
    if (actionId == "voxel.boolean_swap") return swap_voxel_boolean_target();
    if (actionId == "voxel.boolean_cancel") {
        if (!voxelBoolean_.active()) { set_status("No Boolean preview is open", true); return false; }
        cancel_voxel_boolean();
        return true;
    }
    if (actionId == "voxel.boolean_operands_hide" || actionId == "voxel.boolean_operands_delete" ||
        actionId == "voxel.boolean_operands_keep") {
        set_voxel_boolean_operand_policy(actionId == "voxel.boolean_operands_hide" ? VoxelBooleanOperandPolicy::Hide
                                         : actionId == "voxel.boolean_operands_delete" ? VoxelBooleanOperandPolicy::Delete
                                                                                       : VoxelBooleanOperandPolicy::Keep);
        return true;
    }
    if (actionId == "text3d.cancel") { cancel_text3d_authoring(); return true; }
    if (actionId == "create.prefab_from_selection") {
        if (workspace_.mode() != EditorMode::Edit || playSession_.active()) {
            set_status("Stop Play or Simulate before creating a prefab", true);
            return false;
        }
        if (workspace_.selection_count() == 0U) {
            set_status("Select one or more objects to capture as a prefab", true);
            return false;
        }
        const std::set<EditorObjectId> selected(
            workspace_.selected_objects().begin(), workspace_.selected_objects().end());
        std::vector<EditorObjectId> roots;
        for (const EditorObjectId id : workspace_.selected_objects()) {
            const EditorObject* object = workspace_.document().find_object(id);
            if (!object) continue;
            bool selectedAncestor{};
            for (auto parent = object->parent; parent;) {
                if (selected.contains(*parent)) { selectedAncestor = true; break; }
                const EditorObject* ancestor = workspace_.document().find_object(*parent);
                parent = ancestor ? ancestor->parent : std::nullopt;
            }
            if (!selectedAncestor) roots.push_back(id);
        }
        if (roots.empty()) {
            set_status("The selected prefab roots no longer exist", true);
            return false;
        }
        const EditorObject* first = workspace_.document().find_object(roots.front());
        const std::string base = prefab_filename_component(first ? first->name : "Prefab");
        const std::filesystem::path directory = projectRoot_ / "assets" / "prefabs";
        std::error_code directoryError;
        std::filesystem::create_directories(directory, directoryError);
        if (directoryError) {
            set_status("Could not create the prefab asset directory", true);
            return false;
        }
        std::filesystem::path path = directory / (base + ".dveprefab");
        for (std::uint32_t suffix = 2U; std::filesystem::exists(path); ++suffix)
            path = directory / (base + "_" + std::to_string(suffix) + ".dveprefab");
        const auto captured = capture_editor_prefab(
            workspace_.document(), roots, path, first ? first->name : base);
        if (!captured.success) {
            set_status(captured.error.empty() ? "Prefab capture failed" : captured.error, true);
            return false;
        }
        (void)refresh_asset_database(true);
        std::error_code relativeError;
        const auto relative = std::filesystem::relative(path, projectRoot_, relativeError);
        if (!relativeError) {
            if (const EditorAssetRecord* record = assetDatabase_.find_path(relative))
                assetBrowserState_.selectedId = record->id;
        }
        bottomTab_ = BottomPanelTab::Assets;
        recompute_layout();
        set_status("Created prefab " + path.filename().generic_string() + " with " +
                   std::to_string(captured.objectCount) + " objects", false, 6.0F);
        return true;
    }
    if (actionId == "asset.instantiate_prefab") {
        if (workspace_.mode() != EditorMode::Edit || playSession_.active()) {
            set_status("Stop Play or Simulate before instantiating a prefab", true);
            return false;
        }
        if (!assetBrowserState_.selectedId) {
            set_status("Select a prefab asset first", true);
            return false;
        }
        const EditorAssetRecord* asset = std::as_const(assetDatabase_).find(*assetBrowserState_.selectedId);
        if (!asset || asset->kind != EditorAssetKind::Prefab) {
            set_status("The selected asset is not a prefab", true);
            return false;
        }
        std::string error;
        auto prefab = load_editor_prefab(projectRoot_ / asset->relativePath, &error);
        if (!prefab) {
            set_status(error.empty() ? "Could not load prefab" : error, true);
            return false;
        }
        auto command = std::make_unique<InstantiateEditorPrefabCommand>(
            std::move(*prefab), make_rigid_transform(camera_.target, {}));
        auto* commandResult = command.get();
        const CommandResult executed = workspace_.commands().execute(
            workspace_.document(), std::move(command));
        if (!executed.success) {
            set_status(executed.message, true);
            return false;
        }
        workspace_.clear_selection();
        for (const EditorObjectId root : commandResult->result().rootObjectIds)
            workspace_.add_to_selection(root);
        recompute_layout();
        set_status("Instantiated prefab with " +
                   std::to_string(commandResult->result().objectIds.size()) + " objects");
        return true;
    }
    if (actionId == "asset.open") {
        if (!assetBrowserState_.selectedId) {
            set_status("Select an asset to open first", true);
            return false;
        }
        const EditorAssetRecord* asset = std::as_const(assetDatabase_).find(*assetBrowserState_.selectedId);
        if (!asset) {
            set_status("Selected asset no longer exists", true);
            return false;
        }
        const auto extension = asset->relativePath.extension();
        if (asset->kind == EditorAssetKind::Texture || asset->kind == EditorAssetKind::Sprite ||
            extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".dvesprite") {
            std::string error;
            const std::filesystem::path path = projectRoot_ / asset->relativePath;
            const bool opened = extension == ".dvesprite"
                ? spriteAuthoringPanel_.open_asset(path, projectRoot_, &error)
                : spriteAuthoringPanel_.open_texture(path, path.parent_path() /
                    (path.stem().string() + ".dvesprite"), &error);
            if (!opened) {
                set_status(error.empty() ? "Could not open sprite asset" : error, true, 7.0F);
                return false;
            }
            set_status(extension == ".dvesprite" ? "Sprite asset opened" : "Texture opened in Sprite Editor",
                       false, 5.0F);
            refresh_menu_state();
            return true;
        }
        if (extension != ".dverig" && extension != ".dverigui") {
            set_status("The selected asset cannot be opened by a native authoring panel", true);
            return false;
        }
        ControlRigAssetPaths paths;
        paths.rig = projectRoot_ / asset->relativePath;
        if (extension == ".dverigui") paths.rig.replace_extension(".dverig");
        paths.layout = paths.rig;
        paths.layout.replace_extension(".dverigui");
        paths.skeleton = paths.rig;
        paths.skeleton.replace_extension(".dveskeleton");
        if (!std::filesystem::exists(paths.skeleton)) {
            const auto directory = asset->relativePath.parent_path();
            const EditorAssetRecord* candidate = nullptr;
            for (const auto& record : assetDatabase_.records()) {
                if (record.relativePath.parent_path() != directory || record.relativePath.extension() != ".dveskeleton") continue;
                if (candidate) { candidate = nullptr; break; }
                candidate = &record;
            }
            if (candidate) paths.skeleton = projectRoot_ / candidate->relativePath;
        }
        std::string error;
        if (!controlRigPanel_.open_asset(std::move(paths), true, &error)) {
            set_status(error.empty() ? "Could not open Control Rig asset" : error, true, 7.0F);
            return false;
        }
        set_status("Control Rig asset opened", false, 5.0F);
        refresh_menu_state();
        return true;
    }
    if (actionId == "create.empty" || actionId == "create.voxel") {
        const EditorObjectId id = workspace_.document().allocate_object_id();
        EditorObject object(id, actionId == "create.empty" ? "Empty Object" : "Voxel Object");
        object.transform = make_rigid_transform(camera_.target, {});
        if (actionId == "create.voxel") {
            for (int x = 0; x < 4; ++x)
                for (int y = 0; y < 4; ++y)
                    for (int z = 0; z < 4; ++z) object.voxels->set_voxel({x, y, z}, activeMaterial_);
        }
        const CommandResult result = workspace_.commands().execute(
            workspace_.document(), std::make_unique<AddObjectCommand>(std::move(object)));
        if (!result.success) {
            set_status(result.message, true);
            return false;
        }
        workspace_.select_object(id);
        recompute_layout();
        set_status(actionId == "create.empty" ? "Created empty object" : "Created voxel object");
        return true;
    }
    if (actionId == "window.toggle_hierarchy") {
        const bool visible = !workspace_.panel_visible(PanelId::SceneHierarchy);
        workspace_.set_panel_visible(PanelId::SceneHierarchy, visible);
        recompute_layout();
        set_status(visible ? "Scene Hierarchy shown" : "Scene Hierarchy hidden");
        return true;
    }
    if (actionId == "window.toggle_inspector") {
        const bool visible = !workspace_.panel_visible(PanelId::Inspector);
        workspace_.set_panel_visible(PanelId::Inspector, visible);
        recompute_layout();
        set_status(visible ? "Inspector shown" : "Inspector hidden");
        return true;
    }
    if (actionId == "edit.duplicate_special") return duplicateSpecial_.active ? apply_duplicate_special() : open_duplicate_special();
    if (actionId == "edit.duplicate") {
        if (workspace_.selection_count() == 0) {
            set_status("Nothing selected to duplicate", true);
            return false;
        }
        const std::vector<EditorObjectId> ids(workspace_.selected_objects().begin(), workspace_.selected_objects().end());
        const std::size_t count = collect_editor_object_subtree_ids(workspace_.document(), ids).size();
        const CommandResult result = duplicate_objects(ids, Float3{0.5F, 0.0F, 0.5F});
        set_status(result.success ? "Duplicated " + std::to_string(count) + (count == 1 ? " object" : " objects")
                                   : result.message,
                   !result.success);
        return result.success;
    }
    if (actionId == "edit.mirror_x" || actionId == "edit.mirror_y" || actionId == "edit.mirror_z" ||
        actionId == "edit.mirror_copy_x" || actionId == "edit.mirror_copy_y" || actionId == "edit.mirror_copy_z")
        return mirror_selection(actionId.back() == 'x' ? 0 : actionId.back() == 'y' ? 1 : 2, actionId.find("copy") != std::string_view::npos);
    if (actionId == "edit.rename") {
        const auto id = workspace_.selected_object();
        if (!id) {
            set_status("Select an object to rename", true);
            return false;
        }
        const EditorObject* object = workspace_.document().find_object(*id);
        if (!object) return false;
        begin_text_edit(TextEditKind::ObjectName, *id, object->name);
        return true;
    }
    if (actionId == "select.walk_up") return pick_walk(WalkDirection::Up);
    if (actionId == "select.walk_down") return pick_walk(WalkDirection::Down);
    if (actionId == "select.walk_left") return pick_walk(WalkDirection::Left);
    if (actionId == "select.walk_right") return pick_walk(WalkDirection::Right);
    if (actionId == "select.parent" || actionId == "select.children" || actionId == "select.siblings") {
        if (gizmoDragging_) { set_status("Finish the active transform first",true); return false; }
        std::set<EditorObjectId> hits;
        const auto active = workspace_.selected_object();
        const auto* object = active ? workspace_.document().find_object(*active) : nullptr;
        if (actionId == "select.parent") {
            if (object && object->parent) hits.insert(*object->parent);
        } else if (actionId == "select.children") {
            for (const auto id : workspace_.selected_objects())
                for (const auto child : workspace_.document().children_of(id)) hits.insert(child);
        } else if (object) {
            const auto siblings = object->parent ? workspace_.document().children_of(*object->parent)
                                                 : workspace_.document().root_objects();
            hits.insert(siblings.begin(),siblings.end());
        }
        std::erase_if(hits,[&](auto id) {
            const auto* candidate = workspace_.document().find_object(id);
            return !candidate || !candidate->flags.visible || is_isolated_out(id);
        });
        if (hits.empty()) { set_status("No visible hierarchy relatives to select",true); return false; }
        set_selection_domain(SelectionDomain::Objects);
        workspace_.apply_selection(hits,SelectionOperation::Replace);
        recompute_layout();
        set_status(std::to_string(hits.size())+" hierarchy objects selected");
        return true;
    }
    if (actionId == "edit.group") {
        if (workspace_.selection_count() == 0) {
            set_status("Nothing selected to group", true);
            return false;
        }
        // Preserve already-selected subtrees: when both a parent and one of its children are
        // selected, only the selected root is reparented. Flattening the child into a sibling
        // would silently change the hierarchy rather than merely grouping the selection.
        const std::set<EditorObjectId> selected(workspace_.selected_objects().begin(),
                                                workspace_.selected_objects().end());
        std::vector<EditorObjectId> roots;
        for (const EditorObjectId id : workspace_.selected_objects()) {
            const EditorObject* object = workspace_.document().find_object(id);
            if (!object) continue;
            bool ancestorSelected = false;
            for (auto parent = object->parent; parent;) {
                if (selected.contains(*parent)) { ancestorSelected = true; break; }
                const EditorObject* ancestor = workspace_.document().find_object(*parent);
                parent = ancestor ? ancestor->parent : std::nullopt;
            }
            if (!ancestorSelected) roots.push_back(id);
        }
        if (roots.empty()) return false;

        const auto beforeSelection = capture_selection_snapshot();
        Float3 centroid{};
        std::optional<EditorObjectId> commonParent;
        bool firstParent = true;
        for (const EditorObjectId id : roots) {
            const EditorObject* object = workspace_.document().find_object(id);
            if (!object) return false;
            centroid = add(centroid, workspace_.document().world_transform(id).value_or(object->transform).position);
            if (firstParent) { commonParent = object->parent; firstParent = false; }
            else if (commonParent != object->parent) commonParent.reset();
        }
        centroid = multiply(centroid, 1.0F / static_cast<float>(roots.size()));
        // The pivot starts at the bottom centre of what is grouped, so the group sits on surfaces
        // and turns about its footprint (Edit Group Pivot changes it).
        {
            EditorObjectBounds combined;
            for (const EditorObjectId id : roots) {
                const EditorObjectBounds bounds = group_bounds(id);
                if (!bounds.valid) continue;
                if (!combined.valid) { combined = bounds; continue; }
                combined.minimum = {std::min(combined.minimum.x, bounds.minimum.x), std::min(combined.minimum.y, bounds.minimum.y),
                                    std::min(combined.minimum.z, bounds.minimum.z)};
                combined.maximum = {std::max(combined.maximum.x, bounds.maximum.x), std::max(combined.maximum.y, bounds.maximum.y),
                                    std::max(combined.maximum.z, bounds.maximum.z)};
            }
            if (combined.valid)
                centroid = {(combined.minimum.x + combined.maximum.x) * 0.5F, combined.minimum.y,
                            (combined.minimum.z + combined.maximum.z) * 0.5F};
        }

        std::set<std::string> names;
        for (const auto& [id, object] : workspace_.document().objects()) {
            (void)id;
            names.insert(object.name);
        }
        std::string groupName = "Group";
        for (unsigned suffix = 2; names.contains(groupName); ++suffix)
            groupName = "Group " + std::to_string(suffix);
        EditorObject group(workspace_.document().allocate_object_id(), groupName);
        group.parent = commonParent;
        group.transform = make_rigid_transform(centroid, {});
        group.flags.structural = false;
        group.flags.collisionEnabled = false;
        group.flags.decorative = true;
        const EditorObjectId groupId = group.id;
        auto compound = std::make_unique<CompoundCommand>("Group objects");
        compound->add(std::make_unique<AddObjectCommand>(std::move(group), "Group objects"));
        std::vector<ObjectReparentChange> reparents;
        for (const EditorObjectId id : roots) {
            const EditorObject* object = workspace_.document().find_object(id);
            if (object) reparents.push_back({id, object->parent, groupId});
        }
        compound->add(std::make_unique<ReparentObjectsCommand>(std::move(reparents), "Group objects"));
        const CommandResult result = workspace_.commands().execute(workspace_.document(), std::move(compound));
        if (result.success) {
            set_selection_domain(SelectionDomain::Objects);
            workspace_.select_object(groupId);
            workspace_.commands().set_last_selection_transition(beforeSelection,capture_selection_snapshot());
            if (!viewportSettings_.isolatedObjects.empty()) viewportSettings_.isolatedObjects.insert(groupId);
            recompute_layout();
        }
        set_status(result.success ? "Grouped selection" : result.message, !result.success);
        return result.success;
    }
    if (actionId == "edit.open_group") return open_selected_groups();
    if (actionId == "edit.close_group") return close_groups_around_selection();
    if (actionId == "edit.close_all_groups") {
        if (openGroups_.empty()) { set_status("No groups are open", true); return false; }
        close_all_groups();
        return true;
    }
    if (actionId == "edit.edit_pivot") return toggle_pivot_editing();
    if (actionId == "edit.pivot_bottom_center" || actionId == "edit.pivot_center")
        return set_group_pivot(actionId == "edit.pivot_bottom_center");
    if (actionId == "edit.ungroup") {
        const auto beforeSelection = capture_selection_snapshot();
        std::vector<EditorObjectId> groupsToDissolve;
        for (const EditorObjectId id : workspace_.selected_objects()) {
            const EditorObject* group = workspace_.document().find_object(id);
            if (!group || workspace_.document().children_of(id).empty()) continue;
            if (group->flags.locked) {
                set_status("Unlock all selected groups before ungrouping", true);
                return false;
            }
            groupsToDissolve.push_back(id);
        }
        if (groupsToDissolve.empty()) {
            set_status("Select a group (an object with children) to ungroup", true);
            return false;
        }

        const std::set<EditorObjectId> dissolveSet(groupsToDissolve.begin(), groupsToDissolve.end());
        std::vector<EditorObjectId> emptyGroupsToDelete;
        for (const EditorObjectId groupId : groupsToDissolve) {
            const EditorObject* group = workspace_.document().find_object(groupId);
            // Delete only empty containers. Owned geometry and authored metadata must survive.
            if (group && (!group->voxels || group->voxels->occupied_voxel_count() == 0) &&
                !group->polygon && !group->text3d && !group->gaborVolume &&
                group->components.empty() && group->anchors.empty() && !group->prefabLink &&
                group->sourceAsset.empty() && group->importRecipe.empty() && group->textFontAsset.empty() &&
                group->tags.empty() && group->groups.empty() && group->layer == 0)
                emptyGroupsToDelete.push_back(groupId);
        }
        const std::set<EditorObjectId> deleteSet(emptyGroupsToDelete.begin(), emptyGroupsToDelete.end());

        // Compute one final reparenting plan from the original hierarchy. This handles nested
        // selected groups without temporarily attaching a surviving child to a group that is
        // deleted later in the same compound command.
        std::vector<ObjectReparentChange> reparents;
        for (const auto& [id, object] : workspace_.document().objects()) {
            if (!object.parent || !dissolveSet.contains(*object.parent) || deleteSet.contains(id)) continue;
            std::optional<EditorObjectId> target = object.parent;
            while (target && dissolveSet.contains(*target)) {
                const EditorObject* parent = workspace_.document().find_object(*target);
                target = parent ? parent->parent : std::nullopt;
            }
            reparents.push_back({id, object.parent, target});
        }

        std::set<EditorObjectId> released;
        for (const auto& change : reparents) released.insert(change.id);
        for (const auto id : workspace_.selected_objects())
            if (!deleteSet.contains(id)) released.insert(id);
        auto compound = std::make_unique<CompoundCommand>("Ungroup");
        if (!reparents.empty())
            compound->add(std::make_unique<ReparentObjectsCommand>(std::move(reparents), "Ungroup"));
        if (!emptyGroupsToDelete.empty())
            compound->add(std::make_unique<RemoveObjectsCommand>(std::move(emptyGroupsToDelete), "Ungroup"));
        const CommandResult result = workspace_.commands().execute(workspace_.document(), std::move(compound));
        if (result.success) {
            set_selection_domain(SelectionDomain::Objects);
            workspace_.apply_selection(released,SelectionOperation::Replace);
            workspace_.commands().set_last_selection_transition(beforeSelection,capture_selection_snapshot());
            if (!viewportSettings_.isolatedObjects.empty())
                viewportSettings_.isolatedObjects.insert(released.begin(),released.end());
            for (const auto id : deleteSet) viewportSettings_.isolatedObjects.erase(id);
            recompute_layout();
        }
        set_status(result.success ? "Ungrouped selection" : result.message, !result.success);
        return result.success;
    }
    if (actionId == "window.toggle_synth") {
        synthPanel_.toggle(audioMixer_.synthesizer());
        set_status(synthPanel_.open() ? "Synthesizer opened" : "Synthesizer closed");
        return true;
    }
    if (actionId == "window.toggle_chiptune") {
        chiptunePanel_.toggle(audioMixer_);
        set_status(chiptunePanel_.open() ? "Chiptune Tracker opened" : "Chiptune Tracker closed");
        return true;
    }
    if (actionId == "window.toggle_audio") {
        audioPanel_.toggle();
        set_status(audioPanel_.open() ? "Audio Mixer opened" : "Audio Mixer closed");
        return true;
    }
    if (actionId == "window.toggle_audio_event") {
        audioEventPanel_.toggle();
        set_status(audioEventPanel_.open() ? "Audio Event Graph opened" : "Audio Event Graph closed");
        return true;
    }
    if (actionId == "window.toggle_control_rig") {
        controlRigPanel_.toggle();
        set_status(controlRigPanel_.open() ? "Control Rig Editor opened" : "Control Rig Editor closed");
        refresh_menu_state();
        return true;
    }
    if (actionId == "window.toggle_ai_assistant") {
        const bool visible = bottomTab_ != BottomPanelTab::Assistant;
        workspace_.set_panel_visible(PanelId::Assistant, visible);
        bottomTab_ = visible ? BottomPanelTab::Assistant : BottomPanelTab::Console;
        recompute_layout();
        set_status(visible ? "AI Assistant opened" : "AI Assistant hidden");
        return true;
    }
    if (actionId == "window.toggle_live_mcp") {
        if (liveMcpHost_ && liveMcpHost_->status().running) {
            stop_live_mcp_host();
            set_status("Live MCP host stopped");
            return true;
        }
        std::string error;
        if (!start_live_mcp_host({}, &error)) {
            if (!error.empty()) set_status(error, true, 8.0F);
            return false;
        }
        return true;
    }
    if (actionId == "asset.refresh") return start_asset_scan(true);
    if (actionId == "asset.rename") {
        if (!assetBrowserState_.selectedId) { set_status("No asset selected", true); return false; }
        const EditorAssetRecord* asset = std::as_const(assetDatabase_).find(*assetBrowserState_.selectedId);
        if (!asset) { set_status("Selected asset no longer exists", true); return false; }
        bottomTab_ = BottomPanelTab::Assets;
        begin_text_edit(TextEditKind::AssetRename, 0, asset->relativePath.filename().string());
        recompute_layout();
        return true;
    }
    if (actionId == "asset.thumbnail") {
        if (!assetBrowserState_.selectedId) { set_status("No asset selected", true); return false; }
        std::string error;
        (void)finish_asset_scan();
        const bool generated = assetDatabase_.generate_thumbnail(*assetBrowserState_.selectedId, &error);
        set_status(generated ? "Asset thumbnail regenerated" : error, !generated);
        return generated;
    }
    if (actionId == "asset.filter_stale") {
        assetBrowserState_.query.staleOnly = !assetBrowserState_.query.staleOnly;
        assetBrowserState_.firstVisible = 0U;
        bottomTab_ = BottomPanelTab::Assets;
        recompute_layout();
        set_status(assetBrowserState_.query.staleOnly ? "Showing stale and broken assets" : "Showing all assets");
        return true;
    }
    if (actionId == "window.toggle_assets") {
        bottomTab_ = bottomTab_ == BottomPanelTab::Assets ? BottomPanelTab::Console : BottomPanelTab::Assets;
        if (bottomTab_ == BottomPanelTab::Assets && assetDatabase_.records().empty() && !assetScanInFlight_)
            (void)start_asset_scan(false);
        recompute_layout();
        set_status(bottomTab_ == BottomPanelTab::Assets ? "Assets shown" : "Assets hidden");
        return true;
    }
    if (actionId == "view.top" || actionId == "view.front" || actionId == "view.side" || actionId == "view.perspective") {
        const EditorObjectBounds bounds = selection_bounds();
        const Float3 pivot = bounds.valid ? multiply(add(bounds.minimum, bounds.maximum), 0.5F) : camera_.target;
        const float extent = bounds.valid ? length(subtract(bounds.maximum, bounds.minimum)) : 10.0F;
        const float distance = std::clamp(extent * 1.5F, 5.0F, 100.0F);
        camera_.target = pivot;
        if (actionId == "view.perspective") {
            camera_.projection = EditorProjection::Perspective;
            camera_.position = add(pivot, multiply(normalize(Float3{8.0F, 7.0F, 10.0F}), distance));
        } else {
            camera_.projection = EditorProjection::Orthographic;
            camera_.orthographicHeight = std::max(1.0F, distance);
            if (actionId == "view.top") camera_.position = add(pivot, Float3{0.01F, distance, 0.01F});
            else if (actionId == "view.front") camera_.position = add(pivot, Float3{0.0F, 0.0F, distance});
            else camera_.position = add(pivot, Float3{distance, 0.0F, 0.0F});
        }
        set_status(std::string("View: ") + (actionId == "view.top" ? "Top" : actionId == "view.front" ? "Front"
                   : actionId == "view.side" ? "Side" : "Perspective"));
        return true;
    }
    if (actionId == "view.increase_snap" || actionId == "view.decrease_snap") {
        static constexpr std::array<float, 7> kSteps{0.01F, 0.05F, 0.10F, 0.25F, 0.5F, 1.0F, 2.0F};
        float& snap = workspace_.preferences().translateSnapMeters;
        std::size_t nearest = 0;
        for (std::size_t i = 1; i < kSteps.size(); ++i)
            if (std::abs(kSteps[i] - snap) < std::abs(kSteps[nearest] - snap)) nearest = i;
        if (actionId == "view.increase_snap") nearest = std::min(nearest + 1, kSteps.size() - 1);
        else nearest = nearest == 0 ? 0 : nearest - 1;
        snap = kSteps[nearest];
        set_status("Move snap: " + std::to_string(snap).substr(0, 5) + " m");
        return true;
    }
    if (actionId == "view.toggle_move_snap" || actionId == "view.toggle_angle_snap" ||
        actionId == "view.toggle_scale_snap" || actionId == "view.toggle_absolute_grid") {
        EditorPreferences& preferences = workspace_.preferences();
        bool* state = actionId == "view.toggle_move_snap" ? &preferences.translateSnapEnabled
            : actionId == "view.toggle_angle_snap" ? &preferences.rotateSnapEnabled
            : actionId == "view.toggle_scale_snap" ? &preferences.scaleSnapEnabled
            : &preferences.absoluteGridSnap;
        *state = !*state;
        refresh_menu_state();
        set_status(std::string(actionId) + (*state ? " enabled" : " disabled"));
        return true;
    }
    if (actionId == "view.increase_scale_snap" || actionId == "view.decrease_scale_snap") {
        float& step = workspace_.preferences().scaleSnapStep;
        step = std::clamp(step * (actionId == "view.increase_scale_snap" ? 2.0F : 0.5F), 0.01F, 1.0F);
        set_status("Scale snap step: " + std::to_string(step));
        return true;
    }
    if (actionId == "view.increase_angle_snap" || actionId == "view.decrease_angle_snap") {
        static constexpr std::array<float, 6> kSteps{1.0F, 5.0F, 15.0F, 30.0F, 45.0F, 90.0F};
        float& snap = workspace_.preferences().rotateSnapDegrees;
        std::size_t nearest = 0;
        for (std::size_t i = 1; i < kSteps.size(); ++i)
            if (std::abs(kSteps[i] - snap) < std::abs(kSteps[nearest] - snap)) nearest = i;
        if (actionId == "view.increase_angle_snap") nearest = std::min(nearest + 1, kSteps.size() - 1);
        else nearest = nearest == 0 ? 0 : nearest - 1;
        snap = kSteps[nearest];
        set_status("Angle snap: " + std::to_string(snap).substr(0, 5) + "\u00B0");
        return true;
    }
    if (actionId == "help.about") {
        set_status("Destructible Voxel Engine native editor (Linux/X11 authoring host)", false, 6.0F);
        return true;
    }
    if (actionId.starts_with("polygon.")) {
        set_status("Not in this editor yet: polygon mesh editing (vertex/edge/face tools); models import as voxels (File > Import Model)", true, 6.0F);
        return false;
    }
    if (actionId == "audio.import" || actionId == "build.cook" || actionId == "build.package" || actionId == "help.docs") {
        set_status(std::string(actionId == "audio.import" ? kAudioImportUnavailable : actionId == "build.cook" ? kCookUnavailable
                               : actionId == "build.package" ? kPackageUnavailable : kDocsUnavailable), true, 6.0F);
        return false;
    }
    if (actionId.starts_with("audio.") || actionId.starts_with("sequencer.")) {
        set_status("Open the audio editor or sequencer first; this command works inside it", true, 6.0F);
        return false;
    }
    set_status("Not available in this editor build: " + std::string(actionId), true, 6.0F);
    return false;
}

void NativeEditorController::key_down(std::string_view key, bool control, bool shift, bool alt) {
    const std::string normalized = lowercase(key);
    if (!longJob_ && voxel_authoring_key(normalized,control,shift,alt)) { refresh_menu_state(); return; }
    if(!longJob_ && !pending_destructive_confirmation() && textEdit_.kind==TextEditKind::Inactive &&
       !settingsPanel_.open && !commandPaletteOpen_ && mesh_authoring_key(normalized,control,shift,alt)){refresh_menu_state();return;}
    if (polygonPreviewBefore_) { (void)handle_polygon_preview_key(normalized); return; }
    route_play_key(normalized, true);
    if (pending_destructive_confirmation()) {
        if (normalized == "return" || normalized == "enter") confirm_pending_destructive_action();
        else if (normalized == "escape") cancel_pending_destructive_action();
        return;
    }
    if (longJob_) {
        // A long job owns the scene until it finishes: Esc cancels, other keys only explain.
        const bool modifier = normalized.starts_with("shift") || normalized.starts_with("control") ||
                              normalized.starts_with("alt") || normalized.starts_with("super") ||
                              normalized.starts_with("meta") || normalized == "caps_lock";
        if (normalized == "escape") (void)cancel_long_job();
        else if (!modifier) set_status(long_job_busy_reason(), true, 3.0F);
        return;
    }
    if (importOptions_.open) { (void)handle_import_options_key(normalized); return; }
    if (filePicker_.open) { (void)handle_file_picker_key(normalized); return; }
    if (gizmoDragging_ && !control) {
        if (normalized == "enter" || normalized == "return") { finish_gizmo_drag(false); return; }
        if (normalized == "escape") { finish_gizmo_drag(true); return; }
        if (normalized == "backspace") {
            if (!transformNumeric_.empty()) transformNumeric_.pop_back();
            update_gizmo_drag(gizmoLastX_, gizmoLastY_, gizmoLastModifiers_);
            return;
        }
        const char digit = normalized.size() == 1U ? normalized.front() : '\0';
        if ((digit >= '0' && digit <= '9') ||
            ((digit == '.' || normalized == "period") && transformNumeric_.find('.') == std::string::npos) ||
            ((digit == '-' || normalized == "minus") && transformNumeric_.empty())) {
            if (transformNumeric_.size() < 16U) transformNumeric_.push_back(
                normalized == "period" ? '.' : normalized == "minus" ? '-' : digit);
            update_gizmo_drag(gizmoLastX_, gizmoLastY_, gizmoLastModifiers_);
            return;
        }
    }
    // UI zoom hotkeys work in every panel (accessibility), except while a shortcut is being captured.
    if (!shortcutPanel_.capturing && !(settingsPanel_.open && settingsPanel_.valueEditing)) {
        if (const auto direction = ui_zoom_hotkey_direction(normalized, control, alt)) {
            (void)step_ui_zoom(*direction);
            return;
        }
    }
    if (settingsPanel_.open) { handle_settings_key(normalized, control, shift, alt); return; }
    if (shortcutPanel_.open) { handle_shortcut_editor_key(normalized, control, shift, alt); return; }
    if (cinematicCameraPanel_.is_open()) {
        if (!control && !alt && normalized == "escape") {
            cinematicCameraPanel_.close();
            refresh_menu_state();
            set_status("Cinematic Camera Inspector closed");
            return;
        }
        if (control && normalized == "z") {
            if (shift ? cinematicCameraPanel_.redo() : cinematicCameraPanel_.undo())
                set_status(shift ? "Redid cinematic camera edit" : "Undid cinematic camera edit");
            refresh_menu_state();
            return;
        }
        if (control && normalized == "y") {
            if (cinematicCameraPanel_.redo()) set_status("Redid cinematic camera edit");
            refresh_menu_state();
            return;
        }
    }
    if (spritePixelArtOpen_) {
        std::string error;
        if (normalized == "escape" || normalized == "f7") { spritePixelArtOpen_ = false; set_status("Pixel Art Studio closed"); return; }
        if (control && normalized == "s") {
            if (!spritePixelArt_.save(spritePixelArtPath_, &error)) set_status(error, true, 7.0F);
            else set_status("Pixel-art source saved");
            return;
        }
        if (control && normalized == "p") {
            const auto root = projectRoot_.empty() ? std::filesystem::current_path() : projectRoot_;
            SpritePixelArtPublishSettings settings;
            settings.imagePath = root / "assets/sprites/v222_pixel_workspace.png";
            settings.spritePath = root / "assets/sprites/v222_pixel_workspace.dvesprite";
            settings.palettePath = root / "assets/sprites/v222_pixel_workspace.dvepalette";
            settings.imageAssetReference = "v222_pixel_workspace.png";
            settings.paletteAssetReference = "v222_pixel_workspace.dvepalette";
            SpritePixelArtPublishResult result;
            if (!spritePixelArt_.publish(settings, result, &error)) set_status(error, true, 7.0F);
            else set_status("Pixel art published to PNG, palette, and .dvesprite");
            return;
        }
        if (control && normalized == "z") { (void)spritePixelArt_.undo(); return; }
        if (control && normalized == "y") { (void)spritePixelArt_.redo(); return; }
        if (!control && !alt && normalized == "1") { spritePixelArt_.set_tool(SpritePixelTool::Pencil); return; }
        if (!control && !alt && normalized == "2") { spritePixelArt_.set_tool(SpritePixelTool::Eraser); return; }
        if (!control && !alt && normalized == "3") { spritePixelArt_.set_tool(SpritePixelTool::Fill); return; }
        if (!control && !alt && normalized == "n") {
            (void)spritePixelArt_.add_frame("frame_" + std::to_string(spritePixelArt_.document().frames.size()), true, &error);
            if (!error.empty()) set_status(error, true);
            return;
        }
        if (!control && !alt && normalized == "l") {
            (void)spritePixelArt_.add_layer("Layer " + std::to_string(spritePixelArt_.document().frames[spritePixelArt_.selected_frame()].layers.size() + 1U), &error);
            if (!error.empty()) set_status(error, true);
            return;
        }
        if (!control && !alt && normalized == "w") { spritePixelArt_.set_wrap_painting(!spritePixelArt_.wrap_painting()); return; }
        if (!control && !alt && normalized == ",") {
            if (spritePixelArt_.selected_frame() > 0U)
                (void)spritePixelArt_.select_frame(spritePixelArt_.selected_frame() - 1U);
            return;
        }
        if (!control && !alt && normalized == ".") {
            (void)spritePixelArt_.select_frame(std::min(spritePixelArt_.selected_frame() + 1U, spritePixelArt_.document().frames.size() - 1U)); return;
        }
        return;
    }
    if (spriteRig2DOpen_) {
        std::string error;
        if (normalized == "escape" || (normalized == "f7" && shift)) { spriteRig2DOpen_ = false; set_status("Multi-Part Sprite Rig closed"); return; }
        if (control && normalized == "s") {
            if (!spriteRig2D_.save(spriteRig2DPath_, &error)) set_status(error, true, 7.0F);
            else set_status("Sprite rig saved");
            return;
        }
        if (control && normalized == "z") { (void)spriteRig2D_.undo(); return; }
        if (control && normalized == "y") { (void)spriteRig2D_.redo(); return; }
        if (!control && !alt && normalized == "space") { spriteRig2DPreviewPlaying_ = !spriteRig2DPreviewPlaying_; return; }
        if (!control && !alt && normalized == "b") {
            const auto parent = spriteRig2D_.asset().bones.back().id; SpriteRigBoneId id{};
            (void)spriteRig2D_.add_bone("bone_" + std::to_string(spriteRig2D_.asset().bones.size()), parent,
                                       {{8.0F,0.0F},0.0F,{1.0F,1.0F}}, 8.0F, &id, &error);
            if (!error.empty()) set_status(error, true);
            return;
        }
        if (!control && !alt && normalized == "p") {
            SpriteRigPart2D part; part.name = "part_" + std::to_string(spriteRig2D_.asset().parts.size());
            part.bone = spriteRig2D_.asset().bones.back().id; part.spriteAsset = "assets/sprites/v218_sun_route_cast.dvesprite";
            part.clip = "player_idle"; part.drawOrder = static_cast<std::int32_t>(spriteRig2D_.asset().parts.size());
            (void)spriteRig2D_.add_part(part, nullptr, &error);
            if (!error.empty()) set_status(error, true);
            return;
        }
        if (!control && !alt && normalized == "i" && spriteRig2D_.asset().bones.size() >= 3U) {
            const auto& bones = spriteRig2D_.asset().bones; SpriteRigConstraintId id{};
            (void)spriteRig2D_.add_two_bone_ik({0U,"ik_" + std::to_string(spriteRig2D_.asset().constraints.size()),
                bones[bones.size()-2U].id, bones.back().id, {24.0F,16.0F}, 1.0F, 1.0F}, &id, &error);
            if (!error.empty()) set_status(error, true);
            return;
        }
        if (!control && !alt && normalized == "v") {
            SpriteRigVariant2D variant; variant.name = "variant_" + std::to_string(spriteRig2D_.asset().variants.size());
            (void)spriteRig2D_.add_variant(std::move(variant), &error);
            if (!error.empty()) set_status(error, true);
            return;
        }
        return;
    }
    if (tileWorldEditorOpen_) {
        if (normalized == "escape" || normalized == "f8") {
            tileWorldEditorOpen_ = false; set_status("Tile World Editor closed"); return;
        }
        std::string error;
        if (control && normalized == "s") {
            if (!tileWorldEditor_.save(&error)) set_status(error, true, 7.0F);
            else set_status("Tile world and tileset saved");
            return;
        }
        if (control && normalized == "z") {
            if (!tileWorldEditor_.canvas().session().undo(&error)) set_status(error, true);
            return;
        }
        if (control && normalized == "y") {
            if (!tileWorldEditor_.canvas().session().redo(&error)) set_status(error, true);
            return;
        }
        if (!control && !alt && (normalized == "f5" || normalized == "p")) {
            if (!tileWorldEditor_.request_play_test(&error)) { set_status(error, true, 7.0F); return; }
            (void)tileWorldEditor_.consume_play_request();
            tileWorldEditorOpen_ = false;
            (void)dispatch_action("sprite.play_level");
            return;
        }
        if (!control && !alt && normalized.size() == 1U && normalized[0] >= '1' && normalized[0] <= '7') {
            tileWorldEditor_.canvas().set_tool(static_cast<TileMapTool>(normalized[0] - '1'));
            return;
        }
        if (!control && !alt && normalized == "a") {
            tileWorldEditor_.set_show_autotile_rules(!tileWorldEditor_.show_autotile_rules()); return;
        }
        if (!control && !alt && normalized == "v") {
            tileWorldEditor_.set_show_parallax_preview(!tileWorldEditor_.show_parallax_preview()); return;
        }
        if (!control && !alt && normalized == "c") {
            tileWorldEditor_.set_show_chunk_diagnostics(!tileWorldEditor_.show_chunk_diagnostics()); return;
        }
        return;
    }
    if (spriteAnimationGraphOpen_) {
        if (normalized == "escape") { spriteAnimationGraphOpen_ = false; set_status("Sprite animation graph closed"); return; }
        std::string error;
        if (!control && !alt && spriteAnimationGraph_.selected_transition()) {
            const std::size_t index = *spriteAnimationGraph_.selected_transition();
            SpriteAnimationTransition transition = spriteAnimationGraph_.session().asset().transitions[index];
            bool changed = true;
            if (normalized == "[") --transition.priority;
            else if (normalized == "]") ++transition.priority;
            else if (normalized == "b") transition.blendCurve = static_cast<SpriteAnimationBlendCurve>(
                (static_cast<unsigned>(transition.blendCurve) + 1U) % 4U);
            else if (normalized == "p") transition.trackPolicy = static_cast<SpriteAnimationBlendTrackPolicy>(
                (static_cast<unsigned>(transition.trackPolicy) + 1U) % 4U);
            else if (normalized == "e") transition.eventPolicy = static_cast<SpriteAnimationBlendEventPolicy>(
                (static_cast<unsigned>(transition.eventPolicy) + 1U) % 3U);
            else if (normalized == "m") transition.rootMotionPolicy = static_cast<SpriteAnimationBlendRootMotionPolicy>(
                (static_cast<unsigned>(transition.rootMotionPolicy) + 1U) % 3U);
            else changed = false;
            if (changed) {
                if (!spriteAnimationGraph_.inspect_transition(index, transition, &error)) set_status(error, true);
                return;
            }
        }
        if (!control && !alt && normalized == "c") {
            (void)spriteAnimationGraph_.add_comment({120.0F, 80.0F}, {240.0F, 92.0F}, "Graph comment", nullptr, &error);
            if (!error.empty()) set_status(error, true);
            return;
        }
        if (!control && !alt && normalized == "g" && !spriteAnimationGraph_.selected_states().empty()) {
            (void)spriteAnimationGraph_.add_group("State Group", spriteAnimationGraph_.selected_states(), nullptr, &error);
            if (!error.empty()) set_status(error, true);
            return;
        }
        if (!control && !alt && normalized == "s" && !spriteAnimationGraph_.selected_states().empty()) {
            (void)spriteAnimationGraph_.create_subgraph("Subgraph", spriteAnimationGraph_.selected_states(), &error);
            if (!error.empty()) set_status(error, true);
            return;
        }
        if (spriteAnimationGraph_.key_down(normalized, control, shift, &error)) return;
        if (!error.empty()) set_status(error, true);
        return;
    }
    if (spriteLevelPlaying_ && !control && !alt) {
        if (normalized == "escape" || normalized == "f9") {
            (void)dispatch_action("sprite.play_level");
            return;
        }
        if (normalized == "left" || normalized == "a") { spriteLevelInput_.moveX = -1.0F; return; }
        if (normalized == "right" || normalized == "d") { spriteLevelInput_.moveX = 1.0F; return; }
        if (normalized == "space" || normalized == "up" || normalized == "w") {
            spriteLevelInput_.jumpPressed = true;
            spriteLevelInput_.jumpHeld = true;
            return;
        }
        if (normalized == "x" || normalized == "f") { spriteLevelInput_.firePressed = true; return; }
        if (normalized == "r") { spriteLevelInput_.restartPressed = true; return; }
        if (normalized == "f5") { spriteLevelInput_.savePressed = true; return; }
        if (normalized == "f7") { spriteLevelInput_.loadPressed = true; return; }
    }
    if (spriteAuthoringPanel_.open() && spriteAuthoringPanel_.key_down(normalized, control, shift, alt)) return;
    if (controlRigPanel_.open() && controlRigPanel_.key_down(normalized, control, shift, alt)) return;
    if (textEdit_.kind != TextEditKind::Inactive) {
        if (normalized == "tab" && !control && !alt && textEdit_.kind == TextEditKind::DuplicateSetting) {
            // Duplicate Special: Copies, Offset X/Y/Z, Turn X/Y/Z, Scale, Point X/Y/Z.
            std::vector<std::pair<NumberFieldId, int>> order{{NumberFieldId::DuplicateCopies, 0}};
            for (int axis = 0; axis < 3; ++axis) order.push_back({NumberFieldId::DuplicateOffset, axis});
            for (int axis = 0; axis < 3; ++axis) order.push_back({NumberFieldId::DuplicateRotation, axis});
            order.push_back({NumberFieldId::DuplicateScale, 0});
            if (number_field_available(NumberFieldId::DuplicatePoint))
                for (int axis = 0; axis < 3; ++axis) order.push_back({NumberFieldId::DuplicatePoint, axis});
            const auto here = std::find(order.begin(), order.end(),
                                        std::pair{static_cast<NumberFieldId>(textEdit_.objectId), textEdit_.axis});
            const std::size_t index = here == order.end() ? 0U : static_cast<std::size_t>(here - order.begin());
            const auto next = order[(index + (shift ? order.size() - 1U : 1U)) % order.size()];
            commit_text_edit();
            begin_number_field_edit(next.first, next.second);
            return;
        }
        if (normalized == "tab" && !control && !alt &&
            (textEdit_.kind == TextEditKind::Position || textEdit_.kind == TextEditKind::Rotation ||
             textEdit_.kind == TextEditKind::VoxelSize)) {
            // Tab / Shift+Tab: apply this box and move to the next one (X, Y, Z of Position, then
            // Rotation, then Voxel size when shown).
            std::vector<std::pair<NumberFieldId, int>> order;
            for (int axis = 0; axis < 3; ++axis) order.push_back({NumberFieldId::Position, axis});
            for (int axis = 0; axis < 3; ++axis) order.push_back({NumberFieldId::Rotation, axis});
            if (number_field_available(NumberFieldId::VoxelSize)) order.push_back({NumberFieldId::VoxelSize, 0});
            const NumberFieldId current = textEdit_.kind == TextEditKind::Position ? NumberFieldId::Position
                : textEdit_.kind == TextEditKind::Rotation ? NumberFieldId::Rotation : NumberFieldId::VoxelSize;
            const auto here = std::find(order.begin(), order.end(), std::pair{current, textEdit_.axis});
            const std::size_t index = here == order.end() ? 0U : static_cast<std::size_t>(here - order.begin());
            const auto next = order[(index + (shift ? order.size() - 1U : 1U)) % order.size()];
            commit_text_edit();
            if (number_field_available(next.first)) begin_number_field_edit(next.first, next.second);
            return;
        }
        if (normalized == "escape") { cancel_text_edit(); return; }
        if (normalized == "return" || normalized == "enter") { commit_text_edit(); return; }
        if (normalized == "backspace") {
            if (textEdit_.replaceOnNextInput) {
                textEdit_.buffer.clear();
                textEdit_.replaceOnNextInput = false;
            } else if (!textEdit_.buffer.empty()) {
                std::size_t erase = textEdit_.buffer.size() - 1U;
                while (erase > 0U &&
                       (static_cast<unsigned char>(textEdit_.buffer[erase]) & 0xC0U) == 0x80U) --erase;
                textEdit_.buffer.erase(erase);
            }
            if (textEdit_.kind == TextEditKind::HierarchyFilter) {
                hierarchyFilter_ = textEdit_.buffer;
                hierarchyScroll_ = 0;
                recompute_layout();
            } else if (textEdit_.kind == TextEditKind::AssetSearch) {
                assetBrowserState_.query.text = textEdit_.buffer;
                assetBrowserState_.firstVisible = 0U;
                recompute_layout();
            }
            return;
        }
        return;
    }
    if (duplicateSpecial_.active && !control && !alt) {
        if (normalized == "escape") { cancel_duplicate_special("Duplicate Special cancelled"); return; }
        if (normalized == "return" || normalized == "enter") { (void)apply_duplicate_special(); return; }
    }
    if (chiptunePanel_.open() && chiptunePanel_.key_down(normalized, control, shift, alt, audioMixer_)) return;
    if (audioEventPanel_.open() && audioEventPanel_.key_down(normalized, control, shift, alt)) return;
    if (!control && !alt && synthPanel_.open() && synthPanel_.key_down(normalized, audioMixer_.synthesizer())) return;
    if (commandPaletteOpen_) {
        if (normalized == "escape") {
            commandPaletteOpen_ = false;
            commandPaletteQuery_.clear();
            commandPaletteSelection_ = 0U;
            return;
        }
        const auto results = command_palette_results(64);
        if (normalized == "up") {
            if (!results.empty()) commandPaletteSelection_ =
                commandPaletteSelection_ == 0U ? results.size() - 1U : commandPaletteSelection_ - 1U;
            return;
        }
        if (normalized == "down") {
            if (!results.empty()) commandPaletteSelection_ =
                (commandPaletteSelection_ + 1U) % results.size();
            return;
        }
        if (normalized == "home") { commandPaletteSelection_ = 0U; return; }
        if (normalized == "end") {
            commandPaletteSelection_ = results.empty() ? 0U : results.size() - 1U;
            return;
        }
        if (normalized == "backspace") {
            if (!commandPaletteQuery_.empty()) commandPaletteQuery_.pop_back();
            commandPaletteSelection_ = 0U;
            return;
        }
        if (normalized == "return" || normalized == "enter") {
            (void)activate_command_palette_selection();
            return;
        }
        if (control && normalized == "d" && !results.empty()) {
            const CommandPaletteResult& selected = results[
                std::min(commandPaletteSelection_, results.size() - 1U)];
            if (selected.kind == CommandPaletteResultKind::Command) {
                if (favoriteCommandIds_.contains(selected.id)) favoriteCommandIds_.erase(selected.id);
                else favoriteCommandIds_.insert(selected.id);
                persist_menu_state();
            }
            return;
        }
        // Native hosts normally supply printable text through text_input(), but accepting a
        // one-character key here keeps headless hosts and tests usable without duplicate text.
        if (!control && !alt && normalized.size() == 1U &&
            std::isprint(static_cast<unsigned char>(normalized.front()))) {
            commandPaletteQuery_.push_back(normalized.front());
            commandPaletteSelection_ = 0U;
        } else if (!control && !alt && normalized == "space") {
            commandPaletteQuery_.push_back(' ');
            commandPaletteSelection_ = 0U;
        }
        return;
    }

    if (bottomTab_ == BottomPanelTab::Assets && focusRegion_ == EditorFocusRegion::BottomPanel) {
        if (control && normalized == "r") { (void)start_asset_scan(true); return; }
        if (normalized == "f2") { (void)dispatch_action("asset.rename"); return; }
        const auto rows = asset_browser_rows();
        if (!rows.empty() && (normalized == "up" || normalized == "down" || normalized == "home" || normalized == "end")) {
            std::size_t selectedIndex = 0U;
            if (assetBrowserState_.selectedId) {
                const auto found = std::find_if(rows.begin(), rows.end(), [&](const EditorAssetRecord* row) {
                    return row->id == *assetBrowserState_.selectedId;
                });
                if (found != rows.end()) selectedIndex = static_cast<std::size_t>(std::distance(rows.begin(), found));
            }
            if (normalized == "up") selectedIndex = selectedIndex == 0U ? 0U : selectedIndex - 1U;
            else if (normalized == "down") selectedIndex = std::min(selectedIndex + 1U, rows.size() - 1U);
            else if (normalized == "home") selectedIndex = 0U;
            else selectedIndex = rows.size() - 1U;
            assetBrowserState_.selectedId = rows[selectedIndex]->id;
            refresh_menu_state();
            const std::size_t visible = std::max<std::size_t>(1U, layout_.assetRows.size());
            if (selectedIndex < assetBrowserState_.firstVisible) assetBrowserState_.firstVisible = selectedIndex;
            else if (selectedIndex >= assetBrowserState_.firstVisible + visible)
                assetBrowserState_.firstVisible = selectedIndex - visible + 1U;
            recompute_layout();
            return;
        }
        if ((normalized == "return" || normalized == "enter") && assetBrowserState_.selectedId) {
            if (const EditorAssetRecord* asset = std::as_const(assetDatabase_).find(*assetBrowserState_.selectedId)) {
                if (asset->kind == EditorAssetKind::Prefab) {
                    (void)dispatch_action("asset.instantiate_prefab");
                } else if (asset->kind == EditorAssetKind::Sprite || asset->kind == EditorAssetKind::Texture ||
                           asset->relativePath.extension() == ".dverig" ||
                           asset->relativePath.extension() == ".dverigui") {
                    (void)dispatch_action("asset.open");
                } else {
                    const auto reverse = assetDatabase_.reverse_dependencies_of(asset->id);
                    set_status(asset->relativePath.generic_string() + " | " + std::string(to_string(asset->kind)) +
                               " | references " + std::to_string(reverse.size()), false, 6.0F);
                }
            }
            return;
        }
    }

    if (contextMenu_.open) {
        if (normalized == "escape") { close_context_menu(); return; }
        if (normalized == "up" || normalized == "down") {
            if (!contextMenu_.items.empty()) {
                const int direction = normalized == "down" ? 1 : -1;
                std::size_t index = contextMenu_.hoveredItem.value_or(
                    direction > 0 ? contextMenu_.items.size() - 1U : 0U);
                index = direction > 0 ? (index + 1U) % contextMenu_.items.size()
                                      : (index + contextMenu_.items.size() - 1U) % contextMenu_.items.size();
                contextMenu_.hoveredItem = index;
            }
            return;
        }
        if (normalized == "home" && !contextMenu_.items.empty()) { contextMenu_.hoveredItem = 0U; return; }
        if (normalized == "end" && !contextMenu_.items.empty()) {
            contextMenu_.hoveredItem = contextMenu_.items.size() - 1U;
            return;
        }
        if ((normalized == "return" || normalized == "enter" || normalized == "space") &&
            contextMenu_.hoveredItem && *contextMenu_.hoveredItem < contextMenu_.items.size()) {
            const std::string actionId = contextMenu_.items[*contextMenu_.hoveredItem].actionId;
            close_context_menu();
            (void)dispatch_action(actionId);
            return;
        }
        return;
    }

    if (openMenu_) {
        if (normalized == "escape") { close_top_level_menu(); return; }
        if (normalized == "left") { switch_top_level_menu(-1); return; }
        if (normalized == "right") { switch_top_level_menu(1); return; }
        if (normalized == "up") { move_menu_selection(-1); return; }
        if (normalized == "down") { move_menu_selection(1); return; }
        if (normalized == "home") {
            menuHoveredAction_.reset();
            menuScrollOffset_ = 0U;
            move_menu_selection(1);
            return;
        }
        if (normalized == "end") {
            const auto actions = menu_actions(*openMenu_);
            menuHoveredAction_.reset();
            menuScrollOffset_ = actions.empty() ? 0U : actions.size() - 1U;
            move_menu_selection(-1);
            return;
        }
        if (normalized == "return" || normalized == "enter" || normalized == "space") {
            (void)activate_menu_selection();
            return;
        }
        // Alt+mnemonic opening can be added once labels carry explicit mnemonic metadata.
        return;
    }
    if (voxelSlice_.active() && handle_voxel_slice_key(normalized, control, shift, alt)) return;
    if (voxelBoolean_.active() && handle_voxel_boolean_key(normalized, control, shift, alt)) return;
    if (scatterActive_ && handle_scatter_key(normalized, control, shift, alt)) return;
    if (voxelJoin_.active() && handle_voxel_join_key(normalized, control, shift, alt)) return;

    if (alt && !control && normalized.size() == 1U) {
        const char mnemonic = normalized.front();
        const auto found = std::find_if(kMenuBarNames.begin(), kMenuBarNames.end(), [&](std::string_view name) {
            return !name.empty() && static_cast<char>(std::tolower(static_cast<unsigned char>(name.front()))) == mnemonic;
        });
        if (found != kMenuBarNames.end()) {
            open_top_level_menu(std::string(*found));
            return;
        }
    }

    // Escape and focus traversal are intentionally invariant safety/navigation controls.
    // Every productive command below is profile-driven and can be rebound or removed.
    if (normalized == "escape") {
        if (assetDrag_.armed) cancel_asset_drag("Drag cancelled");
        if (placementMode_ != PlacementTarget::NoTarget) finish_placement(true);
        clear_navigation_input();
        if (numberScrub_.armed) { finish_number_scrub(true); return; }
        close_top_level_menu();
        close_context_menu();
        if (gizmoDragging_) finish_gizmo_drag(true);
        else if (pivotEditing_) { pivotEditing_ = false; refresh_menu_state(); set_status("Pivot editing finished"); }
        finish_voxel_stroke();
        if (floodFillArmed_) { floodFillArmed_ = false; set_status("Flood Fill cancelled"); }
        if (voxelMoveAxis_ != 0) { voxelMoveAxis_ = 0; voxelMoveSteps_ = 0; set_status("Voxel move cancelled"); }
        voxelBrushSelecting_ = false;
        if (brushStroking_) finish_brush_stroke(true);
        brushResizing_ = false;
        return;
    }
    if (normalized == "tab") {
        if (!shift && !control && !alt && focusRegion_==EditorFocusRegion::Viewport && workspace_.mode()==EditorMode::Edit) {
            const auto primary=workspace_.selected_object();
            const auto* object=primary?workspace_.document().find_object(*primary):nullptr;
            if (object && object->polygon && object->flags.visible && !object->flags.locked && !is_isolated_out(object->id)) {
                const std::array<ShortcutContext,1> contexts{ShortcutContext::PolygonEditor};
                const auto resolved=workspace_.shortcuts().resolve(keyboard_shortcut(normalized,false,false,false),contexts,settings_capabilities());
                if (resolved && resolved->actionId=="polygon.component_toggle") { (void)dispatch_action(resolved->actionId); return; }
            }
        }
        focus_next(shift); return;
    }
    if (focusRegion_ == EditorFocusRegion::SceneHierarchy && !control && !shift && !alt &&
        navigate_hierarchy(normalized)) return;
    if (focusRegion_ == EditorFocusRegion::Viewport && alt && !control &&
        (normalized == "up" || normalized == "down" || normalized == "left" || normalized == "right")) {
        // Alt+arrows: the same pick walk as the bare arrows (kept for existing habits).
        (void)pick_walk(normalized == "up" ? WalkDirection::Up : normalized == "down" ? WalkDirection::Down
                        : normalized == "left" ? WalkDirection::Left : WalkDirection::Right);
        return;
    }

    // Fly navigation is a held-command context. It must resolve before ordinary key presses
    // so RMB+W moves the camera while plain W still selects the Move tool.
    const auto contexts = active_shortcut_contexts();
    if (std::find(contexts.begin(), contexts.end(), ShortcutContext::FlyNavigation) != contexts.end()) {
        const ShortcutGesture held = keyboard_shortcut(normalized, control, shift, alt, ShortcutActivation::Hold);
        const auto existing = std::find_if(heldShortcutGestures_.begin(), heldShortcutGestures_.end(),
            [&](const ShortcutGesture& gesture) { return gesture == held; });
        if (existing == heldShortcutGestures_.end()) heldShortcutGestures_.push_back(held);
        const auto resolution = workspace_.shortcuts().resolve(held, contexts, settings_capabilities());
        // Held movement is integrated only by update(); an extra key-down step would
        // bypass acceleration and make travel depend on the operating system's events.
        if (resolution && resolution->actionId.starts_with("camera.fly_")) return;
        if (dispatch_shortcut_gesture(held)) return;
        heldShortcutGestures_.pop_back();
    }
    if (dispatch_shortcut_gesture(keyboard_shortcut(normalized, control, shift, alt))) return;
}

void NativeEditorController::focus_next(bool reverse) noexcept {
    reset_camera_navigation();
    constexpr int count = 6;
    int value = static_cast<int>(focusRegion_);
    value = reverse ? (value + count - 1) % count : (value + 1) % count;
    focusRegion_ = static_cast<EditorFocusRegion>(value);
}

void NativeEditorController::frame_selection() noexcept {
    reset_camera_navigation();
    EditorObjectBounds combined = selection_bounds();
    if (selectionDomain_==SelectionDomain::Polygons && !polygonSelection_.components.empty()) {
        const auto* object=workspace_.document().find_object(polygonSelection_.object);
        if (object && object->polygon) {
            combined={};
            for (auto index:polygon_selected_vertices(*object->polygon,polygonSelection_)) {
                const auto p=voxel_to_world(object->transform,object->voxelScale,object->polygon->vertices[index].position);
                if (!combined.valid) combined={p,p,true};
                else { combined.minimum={std::min(combined.minimum.x,p.x),std::min(combined.minimum.y,p.y),std::min(combined.minimum.z,p.z)};
                    combined.maximum={std::max(combined.maximum.x,p.x),std::max(combined.maximum.y,p.y),std::max(combined.maximum.z,p.z)}; }
            }
        }
    }
    if (!combined.valid) {
        for (const auto& [id, object] : workspace_.document().objects()) {
            // Frame All frames what is shown: visible objects, and only the isolated ones.
            if (!object.flags.visible || is_isolated_out(id)) continue;
            const EditorObjectBounds bounds = object_world_bounds(object);
            if (!bounds.valid) continue;
            if (!combined.valid) combined = bounds;
            else {
                combined.minimum = {std::min(combined.minimum.x,bounds.minimum.x),std::min(combined.minimum.y,bounds.minimum.y),std::min(combined.minimum.z,bounds.minimum.z)};
                combined.maximum = {std::max(combined.maximum.x,bounds.maximum.x),std::max(combined.maximum.y,bounds.maximum.y),std::max(combined.maximum.z,bounds.maximum.z)};
            }
        }
    }
    frame_camera_on_bounds(camera_, combined);
}

CommandResult NativeEditorController::nudge_selection(Float3 worldDelta) {
    if (selectionDomain_==SelectionDomain::Polygons) {
        const bool success=transform_polygon_components(worldDelta,{}, {1,1,1},polygon_selection_centre());
        return success?CommandResult::ok():CommandResult::fail("Select unlocked polygon components to nudge");
    }
    std::vector<ObjectTransformChange> changes = selection_transform_snapshot();
    if (changes.empty()) return CommandResult::fail("no objects selected");
    for (ObjectTransformChange& change : changes) change.after.position = add(change.before.position, worldDelta);
    CommandResult result = workspace_.commands().execute(
        workspace_.document(), std::make_unique<TransformObjectsCommand>(std::move(changes), "Nudge objects"));
    set_status(result.success ? "Nudged selection" : result.message, !result.success);
    return result;
}

// --- Numeric fields: Position / Rotation per axis, Voxel size, Brush radius, Surface offset ----
// (ART-084/085). Typing sets each selected object's value (relative edits apply per object);
// dragging a field's label scrubs it with a live preview and one undo; right-click resets,
// copies or pastes.

namespace {
constexpr float kDegreesPerRadian = 57.29577951308232F;
NumberFieldUnit number_field_unit(NumberFieldId field) {
    switch (field) {
    case NumberFieldId::Rotation:
    case NumberFieldId::DuplicateRotation: return NumberFieldUnit::Degrees;
    case NumberFieldId::BrushRadius:
    case NumberFieldId::DuplicateCopies:
    case NumberFieldId::DuplicateScale: return NumberFieldUnit::Count;
    default: return NumberFieldUnit::Meters;
    }
}
const char* number_field_name(NumberFieldId field) {
    switch (field) {
    case NumberFieldId::Position: return "Position";
    case NumberFieldId::Rotation: return "Rotation";
    case NumberFieldId::VoxelSize: return "Voxel size";
    case NumberFieldId::BrushRadius: return "Brush radius";
    case NumberFieldId::SurfaceOffset: return "Surface offset";
    case NumberFieldId::DuplicateCopies: return "Copies";
    case NumberFieldId::DuplicateOffset: return "Offset";
    case NumberFieldId::DuplicateRotation: return "Turn";
    case NumberFieldId::DuplicateScale: return "Scale";
    case NumberFieldId::DuplicatePoint: return "Point";
    }
    return "";
}
float& axis_of(Float3& value, int axis) { return axis == 0 ? value.x : axis == 1 ? value.y : value.z; }
float axis_of(const Float3& value, int axis) { return axis == 0 ? value.x : axis == 1 ? value.y : value.z; }
const char* axis_name(int axis) { return axis == 0 ? "X" : axis == 1 ? "Y" : "Z"; }
bool per_axis(NumberFieldId field) {
    return field == NumberFieldId::Position || field == NumberFieldId::Rotation || field == NumberFieldId::DuplicateOffset ||
           field == NumberFieldId::DuplicateRotation || field == NumberFieldId::DuplicatePoint;
}
bool duplicate_field(NumberFieldId field) {
    return field == NumberFieldId::DuplicateCopies || field == NumberFieldId::DuplicateOffset || field == NumberFieldId::DuplicateRotation ||
           field == NumberFieldId::DuplicateScale || field == NumberFieldId::DuplicatePoint;
}
// Larger magnitudes lose all float precision (or overflow to inf when cast).
constexpr double kNumberFieldTransformLimit = 1.0e6;
constexpr const char* kNumberFieldLimitMessage = "values must be within \xC2\xB1" "1,000,000";
bool same_transform(const RigidTransform& a, const RigidTransform& b) {
    return a.position.x == b.position.x && a.position.y == b.position.y && a.position.z == b.position.z &&
           a.rotation.x == b.rotation.x && a.rotation.y == b.rotation.y && a.rotation.z == b.rotation.z && a.rotation.w == b.rotation.w;
}
bool plain_voxel_object(const EditorObject& object) {
    return object.voxels && !object.polygon && !object.text3d && !object.gaborVolume && object.voxels->occupied_voxel_count() > 0;
}
}  // namespace

std::vector<EditorObjectId> NativeEditorController::number_field_objects(NumberFieldId field) const {
    std::vector<EditorObjectId> result;
    std::set<EditorObjectId> seen;
    for (const EditorObjectId id : workspace_.selected_objects()) {
        const EditorObject* object = workspace_.document().find_object(id);
        if (!object || !seen.insert(id).second) continue;
        if (field == NumberFieldId::VoxelSize && !plain_voxel_object(*object)) continue;
        result.push_back(id);
    }
    return result;
}

std::string NativeEditorController::number_field_refusal(NumberFieldId field) const {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) return "Stop Play or Simulate before editing values";
    if (longJob_) return long_job_busy_reason();
    if (duplicate_field(field)) return {};  // panel settings, not the objects
    for (const EditorObjectId id : number_field_objects(field)) {
        const EditorObject* object = workspace_.document().find_object(id);
        if (!object) continue;
        if (object->flags.locked) return "Unlock " + object->name + " before editing its " + number_field_name(field);
        if (!object->flags.visible || is_isolated_out(id)) return object->name + " is hidden or isolated out of view";
        if (field == NumberFieldId::VoxelSize && object->prefabLink)
            return object->name + " is part of a prefab instance; unpack it or edit the prefab";
    }
    return {};
}

bool NativeEditorController::number_field_available(NumberFieldId field) const {
    if (duplicate_field(field))
        return duplicateSpecial_.active && (field != NumberFieldId::DuplicatePoint || duplicateSpecial_.settings.pivot == DuplicatePivot::Point);
    if (duplicateSpecial_.active && (field == NumberFieldId::Position || field == NumberFieldId::Rotation || field == NumberFieldId::VoxelSize))
        return false;  // the panel replaces the object details
    switch (field) {
    case NumberFieldId::Position:
    case NumberFieldId::Rotation: return !number_field_objects(field).empty();
    case NumberFieldId::VoxelSize: {
        // Shown for a plain voxel primary (the row sits where the Text/Gabor rows go for those).
        const auto primary = workspace_.selected_object();
        const EditorObject* object = primary ? workspace_.document().find_object(*primary) : nullptr;
        return object && plain_voxel_object(*object);
    }
    case NumberFieldId::BrushRadius:
        return activeTool_ == EditorToolId::AddVoxel || activeTool_ == EditorToolId::RemoveVoxel ||
               activeTool_ == EditorToolId::PaintMaterial ||
               (activeTool_ == EditorToolId::Select && selectionDomain_ == SelectionDomain::Voxels && voxelSelectionBrushEnabled_);
    default: break;
    case NumberFieldId::SurfaceOffset:
        return placementMode_ != PlacementTarget::NoTarget ||
               ((activeTool_ == EditorToolId::Select || activeTool_ == EditorToolId::Translate || activeTool_ == EditorToolId::Rotate) &&
                selectionDomain_ == SelectionDomain::Objects);
    }
    return false;
}

std::string NativeEditorController::number_field_text(NumberFieldId field, int axis) const {
    if (field == NumberFieldId::BrushRadius) return std::to_string(voxelBrushRadius_);
    if (field == NumberFieldId::SurfaceOffset) return format_number_field(placementOffsetMeters_, NumberFieldUnit::Meters);
    if (duplicate_field(field)) {
        const DuplicateSpecialSettings& v = duplicateSpecial_.settings;
        switch (field) {
        case NumberFieldId::DuplicateCopies: return std::to_string(v.copies);
        case NumberFieldId::DuplicateOffset: return format_number_field(axis_of(v.offset, axis), NumberFieldUnit::Meters);
        case NumberFieldId::DuplicateRotation: return format_number_field(axis_of(v.rotationDegrees, axis), NumberFieldUnit::Degrees);
        case NumberFieldId::DuplicatePoint: return format_number_field(axis_of(v.point, axis), NumberFieldUnit::Meters);
        default: {
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, "%.4g", static_cast<double>(v.scale));
            return buffer;
        }
        }
    }
    const auto ids = number_field_objects(field);
    if (ids.empty()) return {};
    std::optional<float> shared;
    for (const EditorObjectId id : ids) {
        const EditorObject* object = workspace_.document().find_object(id);
        float value = 0.0F;
        if (field == NumberFieldId::Position) value = axis_of(object->transform.position, axis);
        else if (field == NumberFieldId::Rotation)
            value = axis_of(multiply(quaternion_to_euler_xyz(object->transform.rotation), kDegreesPerRadian), axis);
        else value = object->voxelSizeMeters;
        if (!shared) shared = value;
        else if (std::abs(*shared - value) > 1.0e-5F) return "\xE2\x80\x94";  // em dash: the objects differ
    }
    return format_number_field(*shared, number_field_unit(field));
}

bool NativeEditorController::set_number_field(NumberFieldId field, int axis, std::string_view text, std::string_view verb) {
    const NumberFieldUnit unit = number_field_unit(field);
    std::string label = std::string(verb) + " " + number_field_name(field) +
        (field == NumberFieldId::Position || field == NumberFieldId::Rotation ? std::string(" ") + axis_name(axis) : std::string());
    if (duplicate_field(field)) {
        DuplicateSpecialSettings& v = duplicateSpecial_.settings;
        const double current = field == NumberFieldId::DuplicateCopies ? v.copies
            : field == NumberFieldId::DuplicateScale ? v.scale
            : field == NumberFieldId::DuplicateOffset ? axis_of(v.offset, axis)
            : field == NumberFieldId::DuplicateRotation ? axis_of(v.rotationDegrees, axis) : axis_of(v.point, axis);
        const NumberFieldParse parsed = parse_number_field(text, unit, current);
        const std::string name = std::string(number_field_name(field)) + (per_axis(field) ? std::string(" ") + axis_name(axis) : std::string());
        if (!parsed.ok) { set_status(name + ": " + parsed.error, true); return false; }
        if (field == NumberFieldId::DuplicateCopies) {
            const long long copies = std::llround(parsed.value);
            if (copies < 1 || copies > static_cast<long long>(kMaxDuplicateCopies)) {
                set_status("Copies: 1 to " + std::to_string(kMaxDuplicateCopies), true);
                return false;
            }
            v.copies = static_cast<std::uint32_t>(copies);
        } else if (field == NumberFieldId::DuplicateScale) {
            if (!(parsed.value > 0.0)) { set_status("Scale must be above 0 (1 keeps the size)", true); return false; }
            v.scale = static_cast<float>(parsed.value);
        } else {
            Float3& target = field == NumberFieldId::DuplicateOffset ? v.offset : field == NumberFieldId::DuplicateRotation ? v.rotationDegrees : v.point;
            axis_of(target, axis) = static_cast<float>(parsed.value);
        }
        set_status(duplicate_special_summary());
        return true;
    }
    if (field == NumberFieldId::BrushRadius || field == NumberFieldId::SurfaceOffset) {
        const double current = field == NumberFieldId::BrushRadius ? voxelBrushRadius_ : placementOffsetMeters_;
        const NumberFieldParse parsed = parse_number_field(text, unit, current);
        if (!parsed.ok) { set_status(std::string(number_field_name(field)) + ": " + parsed.error, true); return false; }
        if (field == NumberFieldId::BrushRadius) {
            voxelBrushRadius_ = static_cast<std::int32_t>(std::clamp<long long>(std::llround(parsed.value), 0, 32));
            set_status("Voxel brush radius: " + std::to_string(voxelBrushRadius_) + (parsed.value > 32.0 ? " (most is 32)" : ""));
        } else {
            placementOffsetMeters_ = static_cast<float>(std::clamp(parsed.value, -10.0, 10.0));
            set_status("Surface offset: " + format_number_field(placementOffsetMeters_, unit) + " m" +
                       (std::abs(parsed.value) > 10.0 ? " (limit is 10 m)" : ""));
        }
        recompute_layout();
        return true;
    }
    const auto ids = number_field_objects(field);
    if (ids.empty()) { set_status(std::string(number_field_name(field)) + ": nothing selected to edit", true); return false; }
    if (const std::string refusal = number_field_refusal(field); !refusal.empty()) { set_status(refusal, true); return false; }
    bool changed = false;
    std::unique_ptr<IEditorCommand> command;
    if (field == NumberFieldId::VoxelSize) {
        std::vector<ObjectVoxelSizeChange> changes;
        for (const EditorObjectId id : ids) {
            const EditorObject* object = workspace_.document().find_object(id);
            const NumberFieldParse parsed = parse_number_field(text, unit, object->voxelSizeMeters);
            if (!parsed.ok) { set_status("Voxel size: " + parsed.error, true); return false; }
            if (!(parsed.value >= 0.001 && parsed.value <= 10.0)) {
                set_status("Voxel size must be between 1 mm and 10 m", true);
                return false;
            }
            changed = changed || static_cast<float>(parsed.value) != object->voxelSizeMeters;
            changes.push_back({id, object->transform, object->transform, object->voxelSizeMeters, static_cast<float>(parsed.value),
                               std::nullopt, std::nullopt});
        }
        command = std::make_unique<ScaleVoxelSizeCommand>(std::move(changes), label);
    } else {
        // "x y z" (three values, spaces or commas) sets all three axes at once.
        std::vector<std::string> parts;
        {
            std::string current;
            for (const char c : text) {
                if (c == ' ' || c == ',' || c == '\t') { if (!current.empty()) parts.push_back(std::move(current)); current.clear(); }
                else current.push_back(c);
            }
            if (!current.empty()) parts.push_back(std::move(current));
        }
        bool triple = parts.size() == 3;
        for (const std::string& part : parts) triple = triple && parse_number_field(part, unit, 0.0).ok;
        std::vector<ObjectTransformChange> changes;
        for (const EditorObjectId id : ids) {
            const EditorObject* object = workspace_.document().find_object(id);
            ObjectTransformChange change{id, object->transform, object->transform};
            if (triple) {
                Float3 values = field == NumberFieldId::Position ? object->transform.position
                                                                 : multiply(quaternion_to_euler_xyz(object->transform.rotation), kDegreesPerRadian);
                for (int a = 0; a < 3; ++a) {
                    const NumberFieldParse parsed = parse_number_field(parts[static_cast<std::size_t>(a)], unit, axis_of(values, a));
                    if (!parsed.ok || std::abs(parsed.value) > kNumberFieldTransformLimit) {
                        set_status(std::string(number_field_name(field)) + " " + axis_name(a) + ": " +
                                   (parsed.ok ? std::string(kNumberFieldLimitMessage) : parsed.error), true);
                        return false;
                    }
                    axis_of(values, a) = static_cast<float>(parsed.value);
                }
                const Float3 old = field == NumberFieldId::Position ? object->transform.position
                                                                    : multiply(quaternion_to_euler_xyz(object->transform.rotation), kDegreesPerRadian);
                const bool same = std::abs(values.x - old.x) <= (field == NumberFieldId::Position ? 0.0F : 1.0e-4F) &&
                                  std::abs(values.y - old.y) <= (field == NumberFieldId::Position ? 0.0F : 1.0e-4F) &&
                                  std::abs(values.z - old.z) <= (field == NumberFieldId::Position ? 0.0F : 1.0e-4F);
                if (same) {}
                else if (field == NumberFieldId::Position) change.after.position = values;
                else change.after.rotation = normalize(quaternion_from_euler_xyz(multiply(values, 1.0F / kDegreesPerRadian)));
            } else if (field == NumberFieldId::Position) {
                const NumberFieldParse parsed = parse_number_field(text, unit, axis_of(object->transform.position, axis));
                if (!parsed.ok || std::abs(parsed.value) > kNumberFieldTransformLimit) {
                    set_status("Position " + std::string(axis_name(axis)) + ": " + (parsed.ok ? std::string(kNumberFieldLimitMessage) : parsed.error), true);
                    return false;
                }
                axis_of(change.after.position, axis) = static_cast<float>(parsed.value);
            } else {
                Float3 euler = multiply(quaternion_to_euler_xyz(object->transform.rotation), kDegreesPerRadian);
                const NumberFieldParse parsed = parse_number_field(text, unit, axis_of(euler, axis));
                if (!parsed.ok || std::abs(parsed.value) > kNumberFieldTransformLimit) {
                    set_status("Rotation " + std::string(axis_name(axis)) + ": " + (parsed.ok ? std::string(kNumberFieldLimitMessage) : parsed.error), true);
                    return false;
                }
                // The Euler round trip is not exact: retyping the shown angle must stay a no-op.
                if (std::abs(parsed.value - static_cast<double>(axis_of(euler, axis))) > 1.0e-4) {
                    axis_of(euler, axis) = static_cast<float>(parsed.value);
                    change.after.rotation = normalize(quaternion_from_euler_xyz(multiply(euler, 1.0F / kDegreesPerRadian)));
                }
            }
            changed = changed || !same_transform(change.before, change.after);
            changes.push_back(change);
        }
        if (triple) label = std::string(verb) + " " + number_field_name(field);
        command = std::make_unique<TransformObjectsCommand>(std::move(changes), label);
    }
    if (!changed) {
        // The same value again: nothing to do and no undo step.
        set_status(std::string(number_field_name(field)) + " unchanged");
        return true;
    }
    const auto selection = capture_selection_snapshot();
    const CommandResult result = workspace_.commands().execute(workspace_.document(), std::move(command));
    if (result.success) workspace_.commands().set_last_selection_transition(selection, selection);  // undo restores it
    set_status(result.success ? label + (ids.size() > 1 ? " on " + std::to_string(ids.size()) + " objects" : std::string()) : result.message,
               !result.success);
    return result.success;
}

bool NativeEditorController::reset_number_field(NumberFieldId field, int axis) {
    return set_number_field(field, axis, field == NumberFieldId::VoxelSize ? "0.1" : "0", "Reset");
}

void NativeEditorController::begin_number_field_edit(NumberFieldId field, int axis) {
    const TextEditKind kind = duplicate_field(field) ? TextEditKind::DuplicateSetting
        : field == NumberFieldId::Position ? TextEditKind::Position
        : field == NumberFieldId::Rotation ? TextEditKind::Rotation
        : field == NumberFieldId::VoxelSize ? TextEditKind::VoxelSize
        : field == NumberFieldId::BrushRadius ? TextEditKind::BrushRadius : TextEditKind::SurfaceOffset;
    std::string initial = number_field_text(field, axis);
    if (initial == "\xE2\x80\x94") initial.clear();
    const auto primary = workspace_.selected_object();
    begin_text_edit(kind, duplicate_field(field) ? static_cast<EditorObjectId>(field) : primary.value_or(EditorObjectId{}), std::move(initial));
    textEdit_.axis = axis;
    focusRegion_ = field == NumberFieldId::BrushRadius || field == NumberFieldId::SurfaceOffset ? EditorFocusRegion::Viewport
                                                                                                 : EditorFocusRegion::Inspector;
    const NumberFieldUnit unit = number_field_unit(field);
    set_status(std::string("Type ") + number_field_name(field) + (per_axis(field) ? std::string(" ") + axis_name(axis) : std::string()) +
               (unit == NumberFieldUnit::Degrees ? " in degrees (rad, turn)" : unit == NumberFieldUnit::Count ? ""
                                                                                                       : " in meters (cm, mm, ft, in)") +
               "; +=, -=, *=, /= change it; Tab moves on, Enter applies, Esc cancels", false, 8.0F);
}

void NativeEditorController::update_number_scrub(int x, std::uint32_t modifiers) {
    NumberScrubState& scrub = numberScrub_;
    const int dx = x - scrub.startX;
    if (!scrub.moved) {
        if (std::abs(dx) < 3) return;
        scrub.moved = true;
        if (textEdit_.kind != TextEditKind::Inactive) commit_text_edit();
    }
    const bool fine = (modifiers & 1U) != 0U;
    const bool snap = (modifiers & 2U) != 0U;
    const EditorPreferences& preferences = workspace_.preferences();
    switch (scrub.field) {
    case NumberFieldId::Position:
    case NumberFieldId::Rotation: {
        const bool rotation = scrub.field == NumberFieldId::Rotation;
        const float perPixel = (rotation ? 0.5F : 0.01F) * (fine ? 0.1F : 1.0F);
        const float step = rotation ? preferences.rotateSnapDegrees : preferences.translateSnapMeters;
        for (ObjectTransformChange& change : scrub.transforms) {
            change.after = change.before;
            if (!rotation) {
                float value = axis_of(change.before.position, scrub.axis) + static_cast<float>(dx) * perPixel;
                if (snap && step > 0.0F) value = std::round(value / step) * step;
                axis_of(change.after.position, scrub.axis) = value;
            } else {
                Float3 euler = multiply(quaternion_to_euler_xyz(change.before.rotation), kDegreesPerRadian);
                float value = axis_of(euler, scrub.axis) + static_cast<float>(dx) * perPixel;
                if (snap && step > 0.0F) value = std::round(value / step) * step;
                axis_of(euler, scrub.axis) = value;
                change.after.rotation = normalize(quaternion_from_euler_xyz(multiply(euler, 1.0F / kDegreesPerRadian)));
            }
        }
        apply_transform_preview(scrub.transforms, true);
        break;
    }
    case NumberFieldId::VoxelSize:
        for (ObjectVoxelSizeChange& change : scrub.sizes) {
            float value = change.beforeVoxelSize * std::pow(fine ? 1.001F : 1.01F, static_cast<float>(dx));
            if (snap) value = std::round(value / 0.005F) * 0.005F;
            change.afterVoxelSize = std::clamp(value, 0.001F, 10.0F);
            if (EditorObject* object = workspace_.document().find_object(change.id)) object->voxelSizeMeters = change.afterVoxelSize;
        }
        break;
    case NumberFieldId::BrushRadius:
        voxelBrushRadius_ = std::clamp(static_cast<std::int32_t>(std::lround(scrub.startScalar + static_cast<float>(dx) / (fine ? 24.0F : 8.0F))), 0, 32);
        break;
    case NumberFieldId::SurfaceOffset: {
        float value = scrub.startScalar + static_cast<float>(dx) * (fine ? 0.001F : 0.01F);
        if (snap) value = std::round(value / 0.05F) * 0.05F;
        placementOffsetMeters_ = std::clamp(value, -10.0F, 10.0F);
        break;
    }
    case NumberFieldId::DuplicateCopies:
        duplicateSpecial_.settings.copies = static_cast<std::uint32_t>(std::clamp<long>(
            std::lround(scrub.startScalar + static_cast<float>(dx) / (fine ? 24.0F : 6.0F)), 1L, static_cast<long>(kMaxDuplicateCopies)));
        break;
    case NumberFieldId::DuplicateScale:
        duplicateSpecial_.settings.scale = std::clamp(scrub.startScalar * std::pow(fine ? 1.0002F : 1.002F, static_cast<float>(dx)), 0.01F, 100.0F);
        if (snap) duplicateSpecial_.settings.scale = std::max(0.01F, std::round(duplicateSpecial_.settings.scale / 0.01F) * 0.01F);
        break;
    case NumberFieldId::DuplicateOffset:
    case NumberFieldId::DuplicatePoint:
    case NumberFieldId::DuplicateRotation: {
        const bool turn = scrub.field == NumberFieldId::DuplicateRotation;
        float value = scrub.startScalar + static_cast<float>(dx) * (turn ? 0.5F : 0.01F) * (fine ? 0.1F : 1.0F);
        const float step = turn ? preferences.rotateSnapDegrees : preferences.translateSnapMeters;
        if (snap && step > 0.0F) value = std::round(value / step) * step;
        DuplicateSpecialSettings& v = duplicateSpecial_.settings;
        axis_of(scrub.field == NumberFieldId::DuplicateOffset ? v.offset : turn ? v.rotationDegrees : v.point, scrub.axis) = value;
        break;
    }
    default: break;
    }
    set_status(std::string("Scrubbing ") + number_field_name(scrub.field) + (per_axis(scrub.field) ? std::string(" ") + axis_name(scrub.axis) : std::string()) +
               ": " + number_field_text(scrub.field, scrub.axis) + "  (Shift fine, Ctrl snap, Esc cancels)", false, 2.0F);
}

void NativeEditorController::finish_number_scrub(bool cancel) {
    NumberScrubState scrub = std::move(numberScrub_);
    numberScrub_ = {};
    if (!scrub.moved) {
        // A click on the label without dragging: type into that field.
        if (!cancel) begin_number_field_edit(scrub.field, scrub.axis);
        return;
    }
    const std::string label = std::string("Scrub ") + number_field_name(scrub.field) +
        (scrub.field == NumberFieldId::Position || scrub.field == NumberFieldId::Rotation ? std::string(" ") + axis_name(scrub.axis) : std::string());
    switch (scrub.field) {
    case NumberFieldId::Position:
    case NumberFieldId::Rotation: {
        apply_transform_preview(scrub.transforms, false);
        if (cancel) { set_status("Scrub cancelled"); return; }
        if (std::all_of(scrub.transforms.begin(), scrub.transforms.end(),
                        [](const ObjectTransformChange& change) { return same_transform(change.before, change.after); })) {
            set_status(label + ": unchanged");  // dragged back to where it started: no undo step
            return;
        }
        const auto selection = capture_selection_snapshot();
        const CommandResult result = workspace_.commands().execute(workspace_.document(),
            std::make_unique<TransformObjectsCommand>(std::move(scrub.transforms), label));
        if (result.success) workspace_.commands().set_last_selection_transition(selection, selection);
        set_status(result.success ? label : result.message, !result.success);
        return;
    }
    case NumberFieldId::VoxelSize:
        for (const ObjectVoxelSizeChange& change : scrub.sizes)
            if (EditorObject* object = workspace_.document().find_object(change.id)) object->voxelSizeMeters = change.beforeVoxelSize;
        if (cancel) { set_status("Scrub cancelled"); return; }
        if (std::all_of(scrub.sizes.begin(), scrub.sizes.end(),
                        [](const ObjectVoxelSizeChange& change) { return change.beforeVoxelSize == change.afterVoxelSize; })) {
            set_status(label + ": unchanged");
            return;
        }
        {
            const auto selection = capture_selection_snapshot();
            const CommandResult result = workspace_.commands().execute(workspace_.document(),
                std::make_unique<ScaleVoxelSizeCommand>(std::move(scrub.sizes), label));
            if (result.success) workspace_.commands().set_last_selection_transition(selection, selection);
            set_status(result.success ? label : result.message, !result.success);
        }
        return;
    case NumberFieldId::BrushRadius:
        if (cancel) voxelBrushRadius_ = static_cast<std::int32_t>(scrub.startScalar);
        set_status(cancel ? "Scrub cancelled" : "Voxel brush radius: " + std::to_string(voxelBrushRadius_));
        return;
    case NumberFieldId::SurfaceOffset:
        if (cancel) placementOffsetMeters_ = scrub.startScalar;
        set_status(cancel ? "Scrub cancelled" : "Surface offset: " + format_number_field(placementOffsetMeters_, NumberFieldUnit::Meters) + " m");
        return;
    default:
        if (cancel) (void)set_number_field(scrub.field, scrub.axis, std::to_string(scrub.startScalar));
        set_status(cancel ? "Scrub cancelled" : duplicate_special_summary());
        return;
    }
}

// --- Duplicate Special: N copies, each offset / turned / scaled from the last -----------------

std::size_t NativeEditorController::duplicate_objects_per_copy() const {
    return collect_editor_object_subtree_ids(workspace_.document(), duplicateSpecial_.roots).size();
}

std::string NativeEditorController::duplicate_special_summary() const {
    const DuplicateSpecialSettings& v = duplicateSpecial_.settings;
    const std::string problem = duplicate_special_problem(v, duplicate_objects_per_copy());
    if (!problem.empty()) return problem;
    return std::to_string(v.copies) + (v.copies == 1 ? " copy" : " copies") + " (" +
           std::to_string(v.copies * duplicate_objects_per_copy()) + " objects); Enter applies, Esc cancels";
}

bool NativeEditorController::open_duplicate_special() {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) { set_status("Stop Play or Simulate before duplicating", true); return false; }
    if (longJob_) { set_status(long_job_busy_reason(), true); return false; }
    if (selectionDomain_ != SelectionDomain::Objects) { set_status("Duplicate Special copies objects: press M to return to objects", true); return false; }
    // Roots only: a selected child of a selected parent comes along with its parent.
    const std::set<EditorObjectId> selected(workspace_.selected_objects().begin(), workspace_.selected_objects().end());
    std::vector<EditorObjectId> roots;
    for (const EditorObjectId id : workspace_.selected_objects()) {
        const EditorObject* object = workspace_.document().find_object(id);
        if (!object) continue;
        bool ancestorSelected = false;
        for (auto parent = object->parent; parent && !ancestorSelected;) {
            ancestorSelected = selected.contains(*parent);
            const EditorObject* ancestor = workspace_.document().find_object(*parent);
            parent = ancestor ? ancestor->parent : std::nullopt;
        }
        if (!ancestorSelected && std::find(roots.begin(), roots.end(), id) == roots.end()) roots.push_back(id);
    }
    if (roots.empty()) { set_status("Select the objects to duplicate first", true); return false; }
    for (const EditorObjectId id : roots) {
        const EditorObject* object = workspace_.document().find_object(id);
        if (!object->flags.visible || is_isolated_out(id)) { set_status(object->name + " is hidden or isolated out of view", true); return false; }
    }
    if (textEdit_.kind != TextEditKind::Inactive) commit_text_edit();
    close_previews_for_voxel_edit("Duplicate Special");
    duplicateSpecial_.active = true;
    duplicateSpecial_.roots = std::move(roots);
    recompute_layout();
    refresh_menu_state();
    set_status("Duplicate Special: " + duplicate_special_summary());
    return true;
}

void NativeEditorController::cancel_duplicate_special(std::string reason) {
    if (!duplicateSpecial_.active) return;
    if (textEdit_.kind == TextEditKind::DuplicateSetting) cancel_text_edit();
    duplicateSpecial_.active = false;
    duplicateSpecial_.roots.clear();
    recompute_layout();
    refresh_menu_state();
    if (!reason.empty()) set_status(std::move(reason));
}

std::vector<NativeEditorController::DuplicatePreviewBox> NativeEditorController::duplicate_special_preview(std::size_t limit) const {
    std::vector<DuplicatePreviewBox> boxes;
    if (!duplicateSpecial_.active) return boxes;
    const DuplicateSpecialSettings& v = duplicateSpecial_.settings;
    if (!duplicate_special_problem(v, duplicate_objects_per_copy()).empty()) return boxes;
    const EditorDocument& document = workspace_.document();
    for (const EditorObjectId root : duplicateSpecial_.roots) {
        const EditorObject* rootObject = document.find_object(root);
        if (!rootObject) continue;
        const RigidTransform rootWorld = document.world_transform(root).value_or(rootObject->transform);
        const auto poses = duplicate_special_poses(rootWorld, v);
        for (const EditorObjectId id : collect_editor_object_subtree_ids(document, std::vector<EditorObjectId>{root})) {
            const EditorObject* object = document.find_object(id);
            if (!object || !object->flags.visible) continue;
            const EditorObjectBounds local = object_world_bounds(*object, RigidTransform{});
            if (!local.valid) continue;
            const RigidTransform inRoot = relative_rigid_transform(rootWorld, document.world_transform(id).value_or(object->transform));
            for (const DuplicateCopyPose& pose : poses) {
                if (boxes.size() >= limit) return boxes;
                const RigidTransform scaledInRoot = make_rigid_transform(multiply(inRoot.position, pose.scale), inRoot.rotation);
                boxes.push_back({compose_rigid_transforms(pose.transform, scaledInRoot), local.minimum, local.maximum, pose.scale});
            }
        }
    }
    return boxes;
}

bool NativeEditorController::apply_duplicate_special() {
    if (!duplicateSpecial_.active) { set_status("Open Edit > Duplicate Special first", true); return false; }
    if (textEdit_.kind == TextEditKind::DuplicateSetting) commit_text_edit();
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) { set_status("Stop Play or Simulate before duplicating", true); return false; }
    if (longJob_) { set_status(long_job_busy_reason(), true); return false; }
    const DuplicateSpecialSettings v = duplicateSpecial_.settings;
    const std::size_t perCopy = duplicate_objects_per_copy();
    if (const std::string problem = duplicate_special_problem(v, perCopy); !problem.empty()) { set_status(problem, true); return false; }
    EditorDocument& document = workspace_.document();
    const auto beforeSelection = capture_selection_snapshot();
    const EditorObject* first = document.find_object(duplicateSpecial_.roots.front());
    if (!first) { cancel_duplicate_special("The objects to duplicate are gone"); return false; }

    auto compound = std::make_unique<CompoundCommand>("Duplicate Special");
    std::optional<EditorObjectId> groupId;
    if (v.group) {
        std::set<std::string> names;
        for (const auto& [id, object] : document.objects()) { (void)id; names.insert(object.name); }
        std::string name = first->name + " x" + std::to_string(v.copies);
        for (unsigned suffix = 2; names.contains(name); ++suffix) name = first->name + " x" + std::to_string(v.copies) + " (" + std::to_string(suffix) + ")";
        EditorObject group(document.allocate_object_id(), name);
        group.parent = first->parent;
        group.transform = make_rigid_transform(document.world_transform(first->id).value_or(first->transform).position, {});
        group.flags.structural = false;
        group.flags.collisionEnabled = false;
        group.flags.decorative = true;
        groupId = group.id;
        compound->add(std::make_unique<AddObjectCommand>(std::move(group), "Duplicate Special group"));
    }
    std::vector<EditorObjectId> added;
    if (groupId) added.push_back(*groupId);
    std::vector<EditorObjectId> rootCopies;
    const int digits = v.copies >= 1000 ? 4 : v.copies >= 100 ? 3 : 2;
    for (const EditorObjectId root : duplicateSpecial_.roots) {
        const EditorObject* rootObject = document.find_object(root);
        if (!rootObject) { cancel_duplicate_special("The objects to duplicate changed"); return false; }
        const RigidTransform rootWorld = document.world_transform(root).value_or(rootObject->transform);
        const auto poses = duplicate_special_poses(rootWorld, v);
        const std::vector<EditorObjectId> subtree = collect_editor_object_subtree_ids(document, std::vector<EditorObjectId>{root});
        for (std::size_t k = 0; k < poses.size(); ++k) {
            const DuplicateCopyPose& pose = poses[k];
            std::map<EditorObjectId, EditorObjectId> idMap;
            for (const EditorObjectId id : subtree) idMap.emplace(id, document.allocate_object_id());
            for (const EditorObjectId id : subtree) {
                const EditorObject* source = document.find_object(id);
                EditorObject copy = clone_editor_object(*source);
                copy.id = idMap.at(id);
                if (id == root) {
                    char number[16];
                    std::snprintf(number, sizeof number, " %0*zu", digits, k + 1);
                    copy.name = source->name + number;
                    copy.parent = groupId ? groupId : source->parent;
                    rootCopies.push_back(copy.id);
                } else {
                    copy.parent = source->parent && idMap.contains(*source->parent) ? std::optional(idMap.at(*source->parent)) : source->parent;
                }
                const RigidTransform inRoot = relative_rigid_transform(rootWorld, document.world_transform(id).value_or(source->transform));
                copy.transform = compose_rigid_transforms(pose.transform, make_rigid_transform(multiply(inRoot.position, pose.scale), inRoot.rotation));
                if (copy.attachment) copy.attachment->localTransform.position = multiply(copy.attachment->localTransform.position, pose.scale);
                if (pose.scale != 1.0F) {
                    // Size: voxel objects by voxel size, polygon / subdivision meshes by their axis scale.
                    if (copy.polygon || copy.subdivision) copy.voxelScale = multiply(copy.voxelScale, pose.scale);
                    else if (copy.voxels && !copy.text3d && !copy.gaborVolume) copy.voxelSizeMeters *= pose.scale;
                }
                added.push_back(copy.id);
                compound->add(std::make_unique<AddObjectCommand>(std::move(copy), "Duplicate Special copy"));
            }
        }
    }
    const CommandResult result = workspace_.commands().execute(document, std::move(compound));
    if (!result.success) { set_status(result.message, true); return false; }
    if (!viewportSettings_.isolatedObjects.empty()) viewportSettings_.isolatedObjects.insert(added.begin(), added.end());
    const std::string sourceName = first->name;
    duplicateSpecial_.active = false;
    duplicateSpecial_.roots.clear();
    workspace_.clear_selection();
    if (groupId) workspace_.select_object(*groupId);
    else for (const EditorObjectId id : rootCopies) workspace_.add_to_selection(id);
    workspace_.commands().set_last_selection_transition(beforeSelection, capture_selection_snapshot());
    recompute_layout();
    refresh_menu_state();
    set_status("Made " + std::to_string(v.copies) + (v.copies == 1 ? " copy of " : " copies of ") + sourceName +
               (groupId ? " in one group" : "") + "; Undo removes them");
    return true;
}

DuplicatePanelLayout NativeEditorController::duplicate_panel_layout() const {
    DuplicatePanelLayout panel;
    if (!duplicateSpecial_.active || layout_.inspector.width <= 0) return panel;
    const UiRect& inspector = layout_.inspector;
    panel.panel = {inspector.x + 1, inspector.y + kInspectorHeaderHeight, inspector.width - 2, inspector.height - kInspectorHeaderHeight};
    const int left = inspector.x + 12;
    const int width = inspector.width - 24;
    int y = inspector.y + kInspectorHeaderHeight + 8;
    panel.titleY = y + 12;
    y += 26;
    constexpr int row = 24;
    const int nameWidth = 66;
    const auto single = [&](NumberFieldId field, int boxWidth) {
        panel.rowNames.push_back({field, {left, y, nameWidth, 18}});
        panel.fields.push_back({field, 0, {left, y, nameWidth, 18}, {left + nameWidth, y, std::min(boxWidth, width - nameWidth), 18}});
        y += row;
    };
    const auto triple = [&](NumberFieldId field) {
        panel.rowNames.push_back({field, {left, y, nameWidth, 18}});
        const int each = (width - nameWidth) / 3;
        for (int axis = 0; axis < 3; ++axis) {
            const int x0 = left + nameWidth + axis * each;
            panel.fields.push_back({field, axis, {x0, y, 14, 18}, {x0 + 14, y, std::max(10, each - 18), 18}});
        }
        y += row;
    };
    single(NumberFieldId::DuplicateCopies, 70);
    triple(NumberFieldId::DuplicateOffset);
    triple(NumberFieldId::DuplicateRotation);
    single(NumberFieldId::DuplicateScale, 70);
    const int half = (width - nameWidth - 4) / 2;
    panel.pivotEach = {left + nameWidth, y, half, 18};
    panel.pivotPoint = {left + nameWidth + half + 4, y, half, 18};
    y += row;
    if (duplicateSpecial_.settings.pivot == DuplicatePivot::Point) {
        triple(NumberFieldId::DuplicatePoint);
        panel.pointOrigin = {left + nameWidth, y, half, 18};
        panel.pointSelection = {left + nameWidth + half + 4, y, half, 18};
    } else {
        panel.spaceLocal = {left + nameWidth, y, half, 18};
        panel.spaceWorld = {left + nameWidth + half + 4, y, half, 18};
    }
    y += row;
    panel.group = {left, y, width, 18};
    y += row + 4;
    const int buttonWidth = (width - 6) / 2;
    panel.apply = {left, y, buttonWidth, 22};
    panel.cancel = {left + buttonWidth + 6, y, buttonWidth, 22};
    panel.noteY = y + 44;
    return panel;
}

std::vector<NumberFieldRect> NativeEditorController::number_field_rects() const {
    std::vector<NumberFieldRect> rects;
    if (duplicateSpecial_.active) rects = duplicate_panel_layout().fields;
    const bool stacked = layout_.inspectorStackedFields;
    const auto axis_row = [&](NumberFieldId field, const UiRect& row) {
        if (row.width <= 0 || row.height < 14 || !number_field_available(field)) return;
        // Name at the left (or on the line above when stacked), then X / Y / Z: a draggable letter and a box.
        const int nameWidth = stacked ? 0 : 66;
        const int top = stacked ? row.y + 15 : row.y - 3;  // value baselines line up with the row name
        const int each = (row.width - nameWidth) / 3;
        for (int axis = 0; axis < 3; ++axis) {
            const int x0 = row.x + nameWidth + axis * each;
            rects.push_back({field, axis, {x0, top, 14, 18}, {x0 + 14, top, std::max(10, each - 18), 18}});
        }
    };
    if (layout_.inspectorFields.size() >= 2 && !scatter_settings_panel_active()) {
        axis_row(NumberFieldId::Position, layout_.inspectorFields[0]);
        axis_row(NumberFieldId::Rotation, layout_.inspectorFields[1]);
        if (layout_.inspectorFields.size() >= 3 && number_field_available(NumberFieldId::VoxelSize)) {
            const UiRect row = layout_.inspectorFields[2];
            if (row.width > 0 && row.height >= 14) {
                const int top = stacked ? row.y + 15 : row.y - 3;  // value baselines line up with the row name
                const UiRect name{row.x, row.y, stacked ? row.width : 80, stacked ? 18 : row.height};
                const int boxX = stacked ? row.x + 14 : row.x + 80;
                rects.push_back({NumberFieldId::VoxelSize, 0, name, {boxX, top, std::min(110, row.x + row.width - boxX), 18}});
            }
        }
    }
    // Tool strip: the brush radius or surface offset for the active tool, under the viewport hint.
    if (layout_.viewport.width > 260 && workspace_.mode() == EditorMode::Edit) {
        int x = layout_.viewport.x + 8;
        const int y = layout_.viewport.y + 24;
        for (const NumberFieldId field : {NumberFieldId::BrushRadius, NumberFieldId::SurfaceOffset}) {
            if (!number_field_available(field)) continue;
            const int nameWidth = field == NumberFieldId::BrushRadius ? 92 : 106;
            const int boxWidth = field == NumberFieldId::BrushRadius ? 44 : 72;
            rects.push_back({field, 0, {x, y, nameWidth, 18}, {x + nameWidth, y, boxWidth, 18}});
            x += nameWidth + boxWidth + 12;
        }
    }
    return rects;
}
InspectorSelectionSummary NativeEditorController::inspector_selection_summary() const {
    InspectorSelectionSummary summary;
    const EditorObject* first = nullptr;
    std::array<bool, 5> baseline{};
    for (const auto id : workspace_.selected_objects()) {
        const auto* object = workspace_.document().find_object(id);
        if (!object) continue;
        const std::array<bool, 5> flags{object->flags.visible, object->flags.locked,
            object->flags.anchored, object->flags.structural, object->flags.collisionEnabled};
        if (!first) {
            first = object; baseline = flags;
            for (std::size_t i = 0; i < flags.size(); ++i)
                summary.flags[i] = flags[i] ? InspectorFlagState::On : InspectorFlagState::Off;
        } else {
            const auto& a = object->transform.position; const auto& b = first->transform.position;
            summary.positionMixed |= a.x != b.x || a.y != b.y || a.z != b.z;
            const auto qa = normalize(object->transform.rotation), qb = normalize(first->transform.rotation);
            // q and -q are the same rotation; Euler representations need not be identical.
            const float alignment = std::abs(qa.x*qb.x + qa.y*qb.y + qa.z*qb.z + qa.w*qb.w);
            summary.rotationMixed |= alignment < 1.0F - 1.0e-6F;
            for (std::size_t i = 0; i < flags.size(); ++i)
                if (flags[i] != baseline[i]) summary.flags[i] = InspectorFlagState::Mixed;
        }
        ++summary.count;
    }
    return summary;
}

CommandResult NativeEditorController::toggle_inspector_flag(std::size_t index) {
    if (index >= 5) return CommandResult::fail("invalid inspector flag");
    const auto summary = inspector_selection_summary();
    if (summary.count == 0) return CommandResult::fail("select an object first");
    // A mixed checkbox first enables every selected object, regardless of primary order.
    const bool target = summary.flags[index] != InspectorFlagState::On;
    std::vector<ObjectFlagsChange> changes;
    for (const auto id : workspace_.selected_objects()) {
        const auto* object = workspace_.document().find_object(id);
        if (!object) continue;
        auto after = object->flags;
        switch (index) {
            case 0: after.visible = target; break;
            case 1: after.locked = target; break;
            case 2: after.anchored = target; break;
            case 3: after.structural = target; break;
            case 4: after.collisionEnabled = target; break;
            default: break;
        }
        changes.push_back({id, object->flags, after});
    }
    const auto result = workspace_.commands().execute(workspace_.document(),
        std::make_unique<SetObjectFlagsBatchCommand>(std::move(changes)));
    set_status(result.success ? "Updated selected object policies" : result.message, !result.success);
    return result;
}

CommandResult NativeEditorController::set_primary_position(Float3 worldPosition) {
    if (!workspace_.selected_object()) return CommandResult::fail("no primary object selected");
    const EditorObject* object = workspace_.document().find_object(*workspace_.selected_object());
    if (!object) return CommandResult::fail("primary object no longer exists");
    return nudge_selection(subtract(worldPosition, object->transform.position));
}

CommandResult NativeEditorController::set_primary_rotation_euler_degrees(Float3 degrees) {
    if (!workspace_.selected_object()) return CommandResult::fail("no primary object selected");
    const EditorObject* primary = workspace_.document().find_object(*workspace_.selected_object());
    if (!primary) return CommandResult::fail("primary object no longer exists");
    constexpr float radiansPerDegree = 0.017453292519943295F;
    const Quaternion target = quaternion_from_euler_xyz(multiply(degrees, radiansPerDegree));
    const Quaternion delta = normalize(multiply(target, conjugate(normalize(primary->transform.rotation))));
    const Float3 pivot = selection_pivot();
    std::vector<ObjectTransformChange> changes = selection_transform_snapshot();
    for (ObjectTransformChange& change : changes) {
        change.after.position = add(pivot, rotate(delta, subtract(change.before.position, pivot)));
        change.after.rotation = normalize(multiply(delta, change.before.rotation));
    }
    CommandResult result = workspace_.commands().execute(
        workspace_.document(), std::make_unique<TransformObjectsCommand>(std::move(changes), "Set rotation"));
    set_status(result.success ? "Updated selection rotation" : result.message, !result.success);
    return result;
}

CommandResult NativeEditorController::rescale_primary_voxel_object(Float3 scale) {
    if (!workspace_.selected_object()) return CommandResult::fail("no primary object selected");
    std::unique_ptr<IEditorCommand> command = make_rescale_voxel_object_command(
        workspace_.document(), *workspace_.selected_object(), scale);
    if (!command) {
        CommandResult result = CommandResult::fail("voxel rescale settings are invalid or the object is locked");
        set_status(result.message, true);
        return result;
    }
    CommandResult result = workspace_.commands().execute(workspace_.document(), std::move(command));
    set_status(result.success ? "Rescaled voxel object; runtime transform remains rigid" : result.message,
               !result.success);
    return result;
}

namespace {
// Objects with occupied voxels (as the Scale tool requires); empties, 3D text and Gabor volumes
// are skipped.
[[nodiscard]] bool voxel_size_scalable(const EditorObject& object) noexcept {
    return object.voxels && object.voxels->brick_count() != 0U && !object.text3d && !object.gaborVolume;
}
} // namespace

std::string NativeEditorController::voxel_size_scale_disabled_reason() const {
    if (workspace_.selected_objects().empty()) return "Select one or more voxel objects first.";
    std::size_t voxelObjects = 0;
    for (EditorObjectId id : workspace_.selected_objects()) {
        const EditorObject* object = workspace_.document().find_object(id);
        if (!object || !voxel_size_scalable(*object)) continue;
        if (object->flags.locked) return "Unlock the selected voxel objects first.";
        ++voxelObjects;
    }
    if (voxelObjects == 0U) return "Scale Voxel Size works on voxel objects; none are selected.";
    return {};
}

CommandResult NativeEditorController::scale_selection_voxel_size(float factor) {
    const std::string reason = voxel_size_scale_disabled_reason();
    if (!reason.empty()) {
        set_status(reason, true);
        return CommandResult::fail(reason);
    }
    if (!std::isfinite(factor) || !(factor > 0.0F)) {
        set_status("Voxel size factor must be a positive number", true);
        return CommandResult::fail("invalid voxel size factor");
    }
    std::vector<ObjectVoxelSizeChange> changes;
    std::size_t skipped = 0;
    // Limit the factor (the same for every object, so relative sizes and pivot distances are kept)
    // so that every resulting voxel size stays within the supported range.
    float applied = factor;
    for (EditorObjectId id : workspace_.selected_objects()) {
        const EditorObject* object = workspace_.document().find_object(id);
        if (!object || !voxel_size_scalable(*object)) { ++skipped; continue; }
        changes.push_back({id, object->transform, object->transform, object->voxelSizeMeters, object->voxelSizeMeters});
        const float size = object->voxelSizeMeters;
        if (!(size > 0.0F) || !std::isfinite(size)) continue;
        applied = std::min(applied, std::max(1.0F, kMaxVoxelSizeMeters / size));
        applied = std::max(applied, std::min(1.0F, kMinVoxelSizeMeters / size));
    }
    if (std::abs(applied - 1.0F) <= 1.0e-6F) {
        const std::string message = factor > 1.0F ? "Voxel size is already at the 10 m maximum"
                                                   : "Voxel size is already at the 1 mm minimum";
        set_status(message, true);
        return CommandResult::fail(message);
    }
    const Float3 pivot = selection_pivot();
    for (ObjectVoxelSizeChange& change : changes) {
        change.after.position = add(pivot, multiply(subtract(change.before.position, pivot), applied));
        change.afterVoxelSize = std::clamp(change.beforeVoxelSize * applied, kMinVoxelSizeMeters, kMaxVoxelSizeMeters);
    }
    const std::size_t scaled = changes.size();
    CommandResult result = workspace_.commands().execute(
        workspace_.document(), std::make_unique<ScaleVoxelSizeCommand>(std::move(changes), "Scale voxel size"));
    if (!result.success) {
        set_status(result.message, true);
        return result;
    }
    std::string factorText = std::to_string(applied);
    while (factorText.size() > 1U && factorText.back() == '0') factorText.pop_back();
    if (!factorText.empty() && factorText.back() == '.') factorText.pop_back();
    std::string message = "Scaled voxel size x" + factorText + " on " + std::to_string(scaled) +
                          " voxel object(s); voxel count unchanged";
    if (std::abs(applied - factor) > 1.0e-6F) message += " (limited to the 1 mm .. 10 m voxel size range)";
    if (skipped != 0U) message += "; skipped " + std::to_string(skipped) + " non-voxel object(s)";
    set_status(message);
    return result;
}

std::vector<ComponentInspectorSection> NativeEditorController::primary_component_sections() const {
    if (!workspace_.selected_object()) return {};
    const EditorObject* object = workspace_.document().find_object(*workspace_.selected_object());
    if (!object) return {};
    static const ComponentTypeRegistry registry = ComponentTypeRegistry::make_default();
    return build_component_inspector(object->components, registry);
}

CommandResult NativeEditorController::add_component_to_primary(std::string_view type) {
    if (!workspace_.selected_object()) return CommandResult::fail("no primary object selected");
    const EditorObjectId objectId = *workspace_.selected_object();
    static const ComponentTypeRegistry registry = ComponentTypeRegistry::make_default();
    std::string error;
    auto component = make_component_from_schema(
        type, workspace_.document().allocate_component_id(objectId), registry, &error);
    if (!component) return CommandResult::fail(error.empty() ? "component type is not registered" : error);
    CommandResult result = workspace_.commands().execute(
        workspace_.document(), std::make_unique<AddComponentCommand>(objectId, std::move(*component)));
    set_status(result.success ? "Added component" : result.message, !result.success);
    return result;
}

CommandResult NativeEditorController::remove_component_from_primary(ComponentId componentId) {
    if (!workspace_.selected_object()) return CommandResult::fail("no primary object selected");
    CommandResult result = workspace_.commands().execute(
        workspace_.document(), std::make_unique<RemoveComponentCommand>(*workspace_.selected_object(), componentId));
    set_status(result.success ? "Removed component" : result.message, !result.success);
    return result;
}

CommandResult NativeEditorController::set_component_enabled_on_primary(ComponentId componentId, bool enabled) {
    if (!workspace_.selected_object()) return CommandResult::fail("no primary object selected");
    const EditorObjectId objectId = *workspace_.selected_object();
    const Component* component = workspace_.document().find_component(objectId, componentId);
    if (!component) return CommandResult::fail("component does not exist");
    CommandResult result = workspace_.commands().execute(
        workspace_.document(), std::make_unique<SetComponentEnabledCommand>(
            objectId, componentId, component->enabled, enabled));
    set_status(result.success ? (enabled ? "Enabled component" : "Disabled component") : result.message,
               !result.success);
    return result;
}

CommandResult NativeEditorController::reorder_component_on_primary(ComponentId componentId, std::size_t newIndex) {
    if (!workspace_.selected_object()) return CommandResult::fail("no primary object selected");
    const EditorObjectId objectId = *workspace_.selected_object();
    const EditorObject* object = workspace_.document().find_object(objectId);
    if (!object || newIndex >= object->components.size()) return CommandResult::fail("component index is out of range");
    const auto it = std::find_if(object->components.begin(), object->components.end(),
        [componentId](const Component& component) { return component.id == componentId; });
    if (it == object->components.end()) return CommandResult::fail("component does not exist");
    const std::size_t before = static_cast<std::size_t>(std::distance(object->components.begin(), it));
    CommandResult result = workspace_.commands().execute(
        workspace_.document(), std::make_unique<ReorderComponentCommand>(
            objectId, componentId, before, newIndex));
    set_status(result.success ? "Reordered component" : result.message, !result.success);
    return result;
}

CommandResult NativeEditorController::set_component_property_text_on_primary(
    ComponentId componentId, std::string property, std::string_view text) {
    if (!workspace_.selected_object()) return CommandResult::fail("no primary object selected");
    const EditorObjectId objectId = *workspace_.selected_object();
    const Component* component = workspace_.document().find_component(objectId, componentId);
    if (!component) return CommandResult::fail("component does not exist");
    const auto it = component->properties.find(property);
    if (it == component->properties.end()) return CommandResult::fail("component property does not exist");
    std::string parseError;
    auto parsed = parse_component_value(text, it->second, &parseError);
    if (!parsed) return CommandResult::fail(parseError);
    CommandResult result = workspace_.commands().execute(
        workspace_.document(), std::make_unique<SetComponentPropertyCommand>(
            objectId, componentId, std::move(property), it->second, std::move(*parsed)));
    set_status(result.success ? "Updated component property" : result.message, !result.success);
    return result;
}

std::uint64_t NativeEditorController::material_density_fingerprint() const noexcept {
    // Diagnostics read material densities; the library is small (tens of entries).
    std::uint64_t h = 1469598103934665603ULL;
    for (MaterialId id = 1; id != 0 && id <= materials_.next_available_id(); ++id) {
        const EditorMaterialEntry* entry = materials_.find(id);
        const std::uint64_t bits = entry == nullptr ? 0xFFFFFFFFULL
            : std::bit_cast<std::uint32_t>(entry->definition.densityKilogramsPerCubicMeter);
        h ^= (static_cast<std::uint64_t>(id) << 32U) ^ bits;
        h *= 1099511628211ULL;
    }
    return h;
}

const EditorSelectionDiagnostics& NativeEditorController::selection_diagnostics() const {
    const std::uint64_t key = editor_scene_render_fingerprint(workspace_.document()) * 31U ^
                              editor_selection_fingerprint(workspace_.selected_objects()) * 17U ^
                              material_density_fingerprint();
    if (!selectionDiagnosticsValid_ || key != selectionDiagnosticsKey_) {
        selectionDiagnostics_ = analyze_editor_selection(workspace_.document(), materials_, workspace_.selected_objects());
        selectionDiagnosticsKey_ = key;
        selectionDiagnosticsValid_ = true;
        ++selectionDiagnosticsRebuilds_;
    }
    return selectionDiagnostics_;
}

std::vector<EditorPolygonDrawTriangle> NativeEditorController::polygon_draw_items() const {
    const auto poses=playSession_.presentation_transforms();
    auto items = build_editor_polygon_draw_list(workspace_.document(),camera_,layout_.viewport,
        workspace_.selected_objects(),viewportSettings_.isolatedObjects,100000,&poses,
        subdivisionCageOnly_ && workspace_.mode()==EditorMode::Edit ? workspace_.selected_object().value_or(0):0);
    const auto hidden=mesh_authoring_hidden_objects();
    std::erase_if(items,[&](const auto& item){return hidden.contains(item.object);});
    if (selectionDomain_ == SelectionDomain::Polygons) for (auto& item:items) {
        item.selected = item.object == polygonSelection_.object && polygonSelection_.mode == PolygonSelectionMode::Face &&
            polygonSelection_.components.contains(item.face);
        if (item.selected) item.color={1.0F,.55F,.15F,1};
    }
    {
        // Live mirror: the other half of each mirrored mesh, drawn from the authored half every frame
        // (preview only; Polygon > Apply Live Mirror makes it real geometry with a welded seam).
        EditorDocument preview("Live mirror preview");
        for (const auto& [id, object] : workspace_.document().objects()) {
            if (!object.polygon || !object.flags.visible || is_isolated_out(id) || hidden.contains(id)) continue;
            const auto axis = live_mirror_axis(id);
            if (!axis) continue;
            EditorObject half(id, object.name);
            half.polygon = *object.polygon;
            half.subdivision = object.subdivision;
            if (!mirror_polygon_mesh(*half.polygon, *axis, half.subdivision ? &*half.subdivision : nullptr, nullptr)) continue;
            half.transform = workspace_.document().world_transform(id).value_or(object.transform);
            half.voxelScale = object.voxelScale;
            preview.add_object(std::move(half));
        }
        if (!preview.objects().empty()) {
            auto mirrorItems = build_editor_polygon_draw_list(preview, camera_, layout_.viewport, {}, {}, 100000);
            for (auto& item : mirrorItems) {
                item.color = {item.color.x * 0.55F + 0.25F, item.color.y * 0.55F + 0.3F, item.color.z * 0.55F + 0.4F, item.color.w};
                item.selected = false;
                item.face = std::numeric_limits<std::uint32_t>::max();  // not pickable as a real face
            }
            items.insert(items.end(), mirrorItems.begin(), mirrorItems.end());
            std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) { return a.depth > b.depth; });
        }
    }
    if (polygonSlicePreview_ && polygonSliceBack_) {
        const auto* source=workspace_.document().find_object(polygonPreviewSelection_.object);
        if (source) {
            EditorDocument preview("Slice back preview");
            EditorObject back(source->id,source->name); back.polygon=*polygonSliceBack_; back.subdivision=source->subdivision;
            if (back.subdivision) *back.subdivision=remap_subdivision_topology(*source->polygon,*back.polygon,*back.subdivision);
            back.transform=source->transform; back.voxelScale=source->voxelScale;
            preview.add_object(std::move(back));
            auto backItems=build_editor_polygon_draw_list(preview,camera_,layout_.viewport,{}, {},100000);
            for (auto& item:backItems) { item.color={.2F,.7F,1.0F,1}; item.selected=false; }
            items.insert(items.end(),backItems.begin(),backItems.end());
            std::stable_sort(items.begin(),items.end(),[](const auto& a,const auto& b) { return a.depth>b.depth; });
        }
    }
    return items;
}

const std::vector<EditorVoxelDrawItem>& NativeEditorController::draw_items() const {
    const auto poses = playSession_.presentation_transforms();
    return drawListCache_.get(workspace_.document(), materials_, camera_, layout_.viewport, viewportSettings_,
                              workspace_.selected_objects(), editor_scene_render_fingerprint(workspace_.document()), &poses);
}

const std::vector<EditorVoxelDrawItem>& NativeEditorController::camera_preview_draw_items(
    const EditorCamera& previewCamera, UiRect previewRect, const EditorViewportSettings& previewSettings) const {
    const auto poses = playSession_.presentation_transforms();
    return previewDrawListCache_.get(workspace_.document(), materials_, previewCamera, previewRect, previewSettings,
                                     workspace_.selected_objects(),
                                     editor_scene_render_fingerprint(workspace_.document()), &poses);
}

std::vector<EditorText3DDrawItem> NativeEditorController::text3d_draw_items() const {
    auto items = build_text3d_draw_list(workspace_.document(), camera_, layout_.viewport,
                                        workspace_.selected_objects());
    const auto& isolated = viewportSettings_.isolatedObjects;
    if (!isolated.empty())
        std::erase_if(items, [&](const EditorText3DDrawItem& item) { return !isolated.contains(item.objectId); });
    return items;
}

std::vector<EditorGaborVolumeDrawItem> NativeEditorController::gabor_volume_draw_items() const {
    auto items = build_gabor_volume_draw_list(workspace_.document(), camera_, layout_.viewport,
                                              workspace_.selected_objects());
    const auto& isolated = viewportSettings_.isolatedObjects;
    if (!isolated.empty())
        std::erase_if(items, [&](const EditorGaborVolumeDrawItem& item) { return !isolated.contains(item.objectId); });
    return items;
}

bool NativeEditorController::is_isolated_out(EditorObjectId id) const noexcept {
    const auto& isolated = viewportSettings_.isolatedObjects;
    return !isolated.empty() && !isolated.contains(id);
}

bool NativeEditorController::toggle_isolation() {
    auto& isolated = viewportSettings_.isolatedObjects;
    if (!isolated.empty()) {
        isolated.clear();
        set_status("Isolation off: all visible objects shown");
        refresh_menu_state();
        return true;
    }
    if (workspace_.selected_objects().empty()) {
        set_status("Select objects to isolate first", true);
        return false;
    }
    // The selection and everything parented under it (an isolated group keeps its children).
    std::set<EditorObjectId> result(workspace_.selected_objects().begin(), workspace_.selected_objects().end());
    for (const auto& [id, object] : workspace_.document().objects()) {
        for (auto parent = object.parent; parent;) {
            if (result.contains(*parent)) { result.insert(id); break; }
            const EditorObject* ancestor = workspace_.document().find_object(*parent);
            parent = ancestor ? ancestor->parent : std::nullopt;
        }
    }
    isolated = std::move(result);
    set_status("Isolated " + std::to_string(isolated.size()) + " object(s); View > Isolate Selection restores the rest");
    refresh_menu_state();
    return true;
}

std::vector<EditorObjectId> NativeEditorController::hierarchy_order() const {
    const auto& document = workspace_.document();
    const auto order = document.hierarchy_preorder();
    std::set<EditorObjectId> included;
    if (!hierarchyFilter_.empty()) {
        // Plain text matches names; key:value terms (type:mesh tag:rock ...) filter by property.
        for (const auto id : objects_matching_filter(hierarchyFilter_)) included.insert(id);
        // Children precede their parents in this pass, so each ancestor is visited once.
        // Filtering temporarily exposes matching paths without modifying collapsed state.
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            const auto* object = document.find_object(*it);
            if (object && object->parent && included.contains(*it)) included.insert(*object->parent);
        }
    }
    std::vector<EditorObjectId> result;
    result.reserve(order.size());
    std::set<EditorObjectId> visible;
    for (const auto id : order) {
        const auto* object = document.find_object(id);
        if (!object) continue;
        if (!hierarchyFilter_.empty()) {
            if (!included.contains(id)) continue;
        } else if (object->parent &&
                   (!visible.contains(*object->parent) || collapsedHierarchy_.contains(*object->parent))) continue;
        visible.insert(id);
        result.push_back(id);
    }
    return result;
}

void NativeEditorController::reset_hierarchy_view() {
    collapsedHierarchy_.clear();
    hierarchyFilter_.clear();
    hierarchyScroll_ = 0;
    hierarchyDrag_ = {};
}

bool NativeEditorController::set_hierarchy_expanded(EditorObjectId id, bool expanded) {
    if (!workspace_.document().find_object(id)) return false;
    if (!hierarchyFilter_.empty()) {
        set_status("Clear the hierarchy filter before collapsing or expanding rows", true);
        return false;
    }
    const auto& objects = workspace_.document().objects();
    const bool hasChildren = std::any_of(objects.begin(), objects.end(), [id](const auto& entry) {
        return entry.second.parent == id;
    });
    if (!hasChildren) return false;
    if (expanded) collapsedHierarchy_.erase(id);
    else collapsedHierarchy_.insert(id);
    recompute_layout();
    return true;
}

void NativeEditorController::scroll_hierarchy_to(EditorObjectId id) {
    recompute_layout();
    const auto order = hierarchy_order();
    const auto found = std::find(order.begin(), order.end(), id);
    if (found == order.end()) return;
    const int index = static_cast<int>(found - order.begin());
    if (index < hierarchyScroll_) hierarchyScroll_ = index;
    else if (index >= hierarchyScroll_ + layout_.hierarchyVisibleRows)
        hierarchyScroll_ = index - layout_.hierarchyVisibleRows + 1;
    recompute_layout();
}

bool NativeEditorController::reveal_selection_in_hierarchy() {
    const auto selected = workspace_.selected_object();
    const auto* object = selected ? workspace_.document().find_object(*selected) : nullptr;
    if (!object) { set_status("Select an object to reveal in the hierarchy", true); return false; }
    if (textEdit_.kind == TextEditKind::HierarchyFilter) textEdit_ = {};
    hierarchyFilter_.clear();
    std::set<EditorObjectId> visited;
    for (auto parent = object->parent; parent && visited.insert(*parent).second;) {
        collapsedHierarchy_.erase(*parent);
        const auto* ancestor = workspace_.document().find_object(*parent);
        parent = ancestor ? ancestor->parent : std::nullopt;
    }
    workspace_.set_panel_visible(PanelId::SceneHierarchy, true);
    focusRegion_ = EditorFocusRegion::SceneHierarchy;
    scroll_hierarchy_to(*selected);
    set_status("Revealed " + object->name + " in hierarchy");
    return true;
}

bool NativeEditorController::navigate_hierarchy(std::string_view key) {
    if (key != "up" && key != "down" && key != "left" && key != "right" && key != "home" && key != "end") return false;
    const auto order = hierarchy_order();
    if (order.empty()) return true;
    auto current = workspace_.selected_object();
    auto found = current ? std::find(order.begin(), order.end(), *current) : order.end();
    // A collapsed branch can still hold the primary selection. Resume from its visible ancestor.
    std::set<EditorObjectId> visited;
    while (found == order.end() && current && visited.insert(*current).second) {
        const auto* object = workspace_.document().find_object(*current);
        current = object ? object->parent : std::nullopt;
        if (current) found = std::find(order.begin(), order.end(), *current);
    }
    EditorObjectId next = found == order.end() ? order.front() : *found;
    if (key == "home") next = order.front();
    else if (key == "end") next = order.back();
    else if (found != order.end()) {
        const auto index = static_cast<std::size_t>(found - order.begin());
        const auto* object = workspace_.document().find_object(*found);
        if (key == "up" && index > 0) next = order[index - 1];
        else if (key == "down" && index + 1 < order.size()) next = order[index + 1];
        else if (key == "left") {
            const bool hasVisibleChild = index + 1 < order.size() &&
                workspace_.document().find_object(order[index + 1])->parent == *found;
            if (hierarchyFilter_.empty() && hasVisibleChild) (void)set_hierarchy_expanded(*found, false);
            else if (object && object->parent && std::find(order.begin(), order.end(), *object->parent) != order.end())
                next = *object->parent;
        } else if (key == "right") {
            if (hierarchyFilter_.empty() && hierarchy_collapsed(*found)) (void)set_hierarchy_expanded(*found, true);
            else if (index + 1 < order.size() && workspace_.document().find_object(order[index + 1])->parent == *found)
                next = order[index + 1];
        }
    }
    workspace_.select_object(next);
    scroll_hierarchy_to(next);
    return true;
}

// The Blender-style starter solid: a 1 m cube of 0.1 m voxels resting on the
// Y=0 grid plane, centered on its local origin in X/Z, in Standard Surface.
void add_starter_cube(EditorDocument& document, Float3 position) {
    EditorObject cube(document.allocate_object_id(), "Starter Cube");
    cube.flags.structural = true;
    cube.flags.collisionEnabled = true;
    cube.transform.position = position;
    for (int y = 0; y < 10; ++y)
        for (int z = -5; z < 5; ++z)
            for (int x = -5; x < 5; ++x)
                cube.voxels->set_voxel({x, y, z}, kDefaultSurfaceMaterial);
    document.add_object(std::move(cube));
}

EditorDocument make_new_scene_document() {
    EditorDocument document("Untitled Scene");
    add_starter_cube(document, {0.0F, 0.0F, 0.0F});
    document.mark_clean();
    return document;
}

EditorDocument make_new_project_document() {
    EditorDocument document("Untitled Project");
    add_starter_cube(document, {0.0F, 0.0F, 0.0F});
    document.mark_clean();
    return document;
}

EditorDocument make_native_editor_demo_document() {
    EditorDocument document("Native Editor Demo");
    EditorObject foundation(1001, "Foundation");
    foundation.flags.anchored = true;
    foundation.voxelSizeMeters = 0.25F;
    for (int z = -8; z <= 8; ++z)
        for (int x = -12; x <= 12; ++x) {
            foundation.voxels->set_voxel({x,0,z}, 1);
            if ((x + z) % 5 == 0) foundation.anchors.insert({x,0,z});
        }
    document.add_object(std::move(foundation));

    EditorObject wall(1002, "Destructible Wall");
    wall.voxelSizeMeters = 0.25F;
    wall.transform.position = {0.0F, 0.25F, 0.0F};
    for (int y = 0; y < 14; ++y)
        for (int x = -10; x <= 10; ++x)
            if (!((x == 0 || x == 1) && y < 7)) wall.voxels->set_voxel({x,y,0}, (x + y) % 7 == 0 ? 2 : 6);
    document.add_object(std::move(wall));

    EditorObject beam(1003, "Steel Beam");
    beam.voxelSizeMeters = 0.25F;
    beam.transform.position = {-2.5F, 3.75F, -0.5F};
    for (int x = 0; x < 21; ++x) beam.voxels->set_voxel({x,0,0}, 4);
    document.add_object(std::move(beam));

    EditorObject crate(1004, "Wood Crate");
    crate.voxelSizeMeters = 0.25F;
    crate.transform.position = {3.0F, 0.25F, 1.5F};
    for (int z = 0; z < 5; ++z)
        for (int y = 0; y < 5; ++y)
            for (int x = 0; x < 5; ++x)
                if (x == 0 || x == 4 || y == 0 || y == 4 || z == 0 || z == 4) crate.voxels->set_voxel({x,y,z}, 3);
    document.add_object(std::move(crate));
    document.mark_clean();
    return document;
}

// ---------------------------------------------------------------------------------------------
// Authored voxel Booleans (ART-060)

// --- Scatter Objects -------------------------------------------------------------------------

namespace {
std::optional<EditorObjectId> enclosing_scatter_group(const EditorDocument& document, EditorObjectId id);
// Top-level objects of `selection` (no selected ancestor), skipping scatter groups.
std::vector<EditorObjectId> scatter_source_roots(const EditorDocument& document, const std::set<EditorObjectId>& selection) {
    std::vector<EditorObjectId> roots;
    for (const EditorObjectId id : selection) {
        const EditorObject* object = document.find_object(id);
        if (!object || is_scatter_group(*object)) continue;
        bool nested = false;
        for (auto parent = object->parent; parent && !nested;) {
            nested = selection.contains(*parent);
            const EditorObject* ancestor = document.find_object(*parent);
            parent = ancestor ? ancestor->parent : std::nullopt;
        }
        if (!nested) roots.push_back(id);
    }
    return roots;
}

bool is_scatter_surface(const EditorObject& object) {
    return object.polygon || (object.voxels && object.voxels->occupied_voxel_count() > 0 && !object.text3d && !object.gaborVolume);
}

// With pinned sources: every selected voxel object that is not part of a source is a surface,
// except hidden objects, objects isolated out of view, and earlier scatter copies (the same
// ground rules as the brush).
std::vector<EditorObjectId> scatter_surfaces_from_selection(const EditorDocument& document,
                                                            const std::set<EditorObjectId>& selection,
                                                            const std::vector<EditorObjectId>& sourceRoots,
                                                            const std::function<bool(EditorObjectId)>& isolatedOut) {
    const std::vector<EditorObjectId> sourceTree = collect_editor_object_subtree_ids(document, sourceRoots);
    const std::set<EditorObjectId> excluded(sourceTree.begin(), sourceTree.end());
    std::vector<EditorObjectId> surfaces;
    for (const EditorObjectId id : selection) {
        const EditorObject* object = document.find_object(id);
        if (!object || excluded.contains(id) || !is_scatter_surface(*object)) continue;
        if (!object->flags.visible || isolatedOut(id) || enclosing_scatter_group(document, id)) continue;
        surfaces.push_back(id);
    }
    return surfaces;
}

std::string scatter_names(const EditorDocument& document, const std::vector<EditorObjectId>& ids, std::size_t limit = 3) {
    std::string text;
    std::size_t shown = 0;
    for (const EditorObjectId id : ids) {
        const EditorObject* object = document.find_object(id);
        if (!object) continue;
        if (shown == limit) { text += ", +" + std::to_string(ids.size() - limit) + " more"; break; }
        text += (text.empty() ? "" : ", ") + object->name;
        ++shown;
    }
    return text.empty() ? std::string("(none)") : text;
}
} // namespace

std::shared_ptr<const EditorPrefabAsset> NativeEditorController::load_selected_scatter_prefab(std::string* error) const {
    if (!assetBrowserState_.selectedId) return nullptr;
    const EditorAssetRecord* asset = std::as_const(assetDatabase_).find(*assetBrowserState_.selectedId);
    if (!asset || asset->kind != EditorAssetKind::Prefab) return nullptr;
    std::string loadError;
    auto prefab = load_editor_prefab(projectRoot_ / asset->relativePath, &loadError);
    if (!prefab) {
        if (error) *error = loadError.empty() ? std::string("could not load the prefab") : loadError;
        return nullptr;
    }
    return std::make_shared<const EditorPrefabAsset>(std::move(*prefab));
}

bool NativeEditorController::set_scatter_sources() {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) {
        set_status("Stop Play or Simulate before setting scatter sources", true);
        return false;
    }
    const std::set<EditorObjectId> selected(workspace_.selected_objects().begin(), workspace_.selected_objects().end());
    std::vector<EditorObjectId> roots = scatter_source_roots(workspace_.document(), selected);
    std::string error;
    std::shared_ptr<const EditorPrefabAsset> prefab = load_selected_scatter_prefab(&error);
    if (!error.empty()) { set_status("Could not load the selected prefab: " + error, true, 6.0F); return false; }
    if (roots.empty() && !prefab) {
        set_status("Select the objects to copy (or a prefab in Assets), then Set Scatter Sources", true, 6.0F);
        return false;
    }
    pinnedScatterRoots_ = std::move(roots);
    pinnedScatterPrefab_ = std::move(prefab);
    if (activeTool_ == EditorToolId::ScatterBrush && !brushStroking_) begin_scatter_brush_session();
    refresh_menu_state();
    std::string names = pinnedScatterRoots_.empty() ? std::string() : scatter_names(workspace_.document(), pinnedScatterRoots_);
    if (pinnedScatterPrefab_) names += (names.empty() ? "prefab " : ", prefab ") + pinnedScatterPrefab_->name;
    set_status("Scatter sources set: " + names + ". Now select the surfaces, then Create > Scatter Objects (or paint)", false, 8.0F);
    return true;
}

void NativeEditorController::clear_scatter_sources() {
    pinnedScatterRoots_.clear();
    pinnedScatterPrefab_.reset();
    refresh_menu_state();
    set_status("Scatter sources cleared: the selection decides again");
}

void NativeEditorController::adopt_scatter_settings() {
    const EditorDocument& document = workspace_.document();
    const auto adopt_from = [&](EditorObjectId groupId) {
        const EditorObject* group = document.find_object(groupId);
        if (!group) return false;
        const auto stored = read_scatter_settings(*group);
        if (!stored) return false;
        scatterSettings_ = *stored;
        return true;
    };
    // A selected scatter group (or a copy inside one) hands over the settings that made it.
    if (const auto active = workspace_.selected_object()) {
        const EditorObject* object = document.find_object(*active);
        std::optional<EditorObjectId> group;
        if (object && is_scatter_group(*object)) group = object->id;
        else if (object) group = enclosing_scatter_group(document, object->id);
        if (group && adopt_from(*group)) { scatterSettingsSceneKey_ = document.path(); return; }
    }
    // The first scatter in a scene continues from its newest scatter group, so settings come
    // back with the scene.
    if (scatterSettingsSceneKey_ == document.path()) return;
    scatterSettingsSceneKey_ = document.path();
    for (auto it = document.objects().rbegin(); it != document.objects().rend(); ++it)
        if (is_scatter_group(it->second) && adopt_from(it->first)) return;
}

std::string NativeEditorController::scatter_disabled_reason() const {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) return "Stop Play or Simulate before scattering.";
    const EditorDocument& document = workspace_.document();
    if (scatter_sources_pinned()) {
        const bool anySource = pinnedScatterPrefab_ || std::any_of(pinnedScatterRoots_.begin(), pinnedScatterRoots_.end(),
            [&](EditorObjectId id) { return document.find_object(id) != nullptr; });
        if (!anySource) return "The scatter sources were deleted: set them again, or Clear Scatter Sources.";
        const std::set<EditorObjectId> selected(workspace_.selected_objects().begin(), workspace_.selected_objects().end());
        if (scatter_surfaces_from_selection(document, selected, pinnedScatterRoots_,
                                            [this](EditorObjectId id) { return is_isolated_out(id); }).empty())
            return "Scatter sources are set: select one or more visible voxel or polygon surfaces to scatter onto.";
        return {};
    }
    const auto target = workspace_.selected_object();
    if (!target) return "Select the objects to scatter, then click the surface to scatter onto last.";
    const EditorObject* surface = document.find_object(*target);
    if (!surface || !is_scatter_surface(*surface))
        return "Click a voxel or polygon object last: the last selected object is the surface to scatter onto.";
    if (!surface->flags.visible || is_isolated_out(surface->id))
        return "The surface is hidden (or isolated out of view): show it before scattering onto it.";
    bool prefabSelected = false;
    if (assetBrowserState_.selectedId) {
        const EditorAssetRecord* asset = std::as_const(assetDatabase_).find(*assetBrowserState_.selectedId);
        prefabSelected = asset && asset->kind == EditorAssetKind::Prefab;
    }
    if (workspace_.selection_count() < 2 && !prefabSelected)
        return "Also select what to scatter: objects in the scene, or a prefab in the Assets panel.";
    return {};
}

std::vector<ScatterSource> NativeEditorController::scatter_sources() const {
    std::vector<ScatterSource> sources;
    for (const EditorObjectId root : scatterSourceRoots_) {
        const EditorObject* object = workspace_.document().find_object(root);
        if (object) sources.push_back({&workspace_.document(), {root}, object->name});
    }
    if (scatterPrefab_)
        sources.push_back({&scatterPrefab_->templateDocument, scatterPrefab_->rootTemplateObjectIds,
                           "prefab " + scatterPrefab_->name});
    return sources;
}

void NativeEditorController::replan_scatter() {
    scatterPlan_ = plan_scatter(workspace_.document(), scatterTargets_, scatter_sources().size(), scatterSettings_);
}

bool NativeEditorController::begin_scatter() {
    floodFillArmed_ = false;  // another tool takes the next viewport click
    const std::string reason = scatter_disabled_reason();
    if (!reason.empty()) { set_status(reason, true, 6.0F); return false; }
    // Scatter, Slice and Boolean previews share the panel slot at the top of the viewport, so
    // only one is open at a time: starting one closes the others.
    if (voxelBoolean_.active()) cancel_voxel_boolean("Boolean preview closed: Scatter started");
    cancel_voxel_slice();
    cancel_voxel_join("Join preview closed: Scatter started");
    adopt_scatter_settings();
    const EditorDocument& document = workspace_.document();
    const std::set<EditorObjectId> selected(workspace_.selected_objects().begin(), workspace_.selected_objects().end());
    scatterTargets_.clear();
    scatterSourceRoots_.clear();
    scatterPrefab_.reset();
    if (scatter_sources_pinned()) {
        for (const EditorObjectId id : pinnedScatterRoots_)
            if (document.find_object(id)) scatterSourceRoots_.push_back(id);
        scatterPrefab_ = pinnedScatterPrefab_;
        scatterTargets_ = scatter_surfaces_from_selection(document, selected, scatterSourceRoots_,
                                                          [this](EditorObjectId id) { return is_isolated_out(id); });
    } else {
        const EditorObjectId target = *workspace_.selected_object();
        scatterTargets_.push_back(target);
        // Sources: the selected roots other than the target. A selected object inside the
        // target's subtree, or an ancestor of it, cannot be copied onto it.
        const std::vector<EditorObjectId> targetSubtree =
            collect_editor_object_subtree_ids(document, std::vector<EditorObjectId>{target});
        std::set<EditorObjectId> others = selected;
        others.erase(target);
        for (const EditorObjectId id : scatter_source_roots(document, others)) {
            if (std::find(targetSubtree.begin(), targetSubtree.end(), id) != targetSubtree.end()) continue;
            const auto sourceTree = collect_editor_object_subtree_ids(document, std::vector<EditorObjectId>{id});
            if (std::find(sourceTree.begin(), sourceTree.end(), target) != sourceTree.end()) continue;
            scatterSourceRoots_.push_back(id);
        }
        std::string error;
        scatterPrefab_ = load_selected_scatter_prefab(&error);
        if (!error.empty()) { set_status("Could not load the selected prefab: " + error, true, 6.0F); return false; }
    }
    if (scatterSourceRoots_.empty() && !scatterPrefab_) {
        set_status("Nothing to scatter: select objects other than the surface, or a prefab", true, 6.0F);
        return false;
    }
    scatterActive_ = true;
    replan_scatter();
    recompute_layout();  // the inspector shows the scatter settings
    refresh_menu_state();
    set_status("Scatter preview: Enter commits, Esc cancels; settings are in the Inspector");
    return true;
}

void NativeEditorController::set_scatter_settings(const ScatterSettings& settings) {
    scatterSettings_ = sanitize_scatter_settings(settings);
    if (scatterActive_) replan_scatter();
}

void NativeEditorController::cancel_scatter(std::string reason) {
    if (!scatterActive_) return;
    scatterActive_ = false;
    scatterPlan_ = {};
    scatterPrefab_.reset();
    scatterSourceRoots_.clear();
    if (textEdit_.kind == TextEditKind::ScatterSetting && !scatter_settings_panel_active()) cancel_text_edit();
    recompute_layout();
    refresh_menu_state();
    set_status(reason.empty() ? "Scatter cancelled; nothing changed" : std::move(reason));
}

bool NativeEditorController::commit_scatter() {
    if (!scatterActive_) { set_status("No scatter preview is open", true); return false; }
    replan_scatter();  // the scene may have changed since the preview was drawn
    ScatterBuildResult build = build_scatter_command(workspace_.document(), scatterTargets_, scatterPlan_,
                                                     scatter_sources(), scatterSettings_);
    if (!build.command) {
        set_status(build.error.empty() ? "Scatter failed" : build.error, true, 6.0F);
        return false;
    }
    const std::size_t copies = scatterPlan_.samples.size();
    const CommandResult result = workspace_.commands().execute(workspace_.document(), std::move(build.command));
    if (!result.success) {
        set_status(result.message, true, 6.0F);
        return false;
    }
    scatterActive_ = false;
    scatterPlan_ = {};
    scatterPrefab_.reset();
    scatterSourceRoots_.clear();
    if (textEdit_.kind == TextEditKind::ScatterSetting && !scatter_settings_panel_active()) cancel_text_edit();
    keep_isolated_with_subtree(build.groupId);
    workspace_.select_object(build.groupId);
    recompute_layout();
    refresh_menu_state();
    set_status("Scattered " + std::to_string(copies) + " copies (" + std::to_string(build.objectCount) +
               " objects) under one group; Undo removes them all");
    return true;
}

std::vector<std::string> NativeEditorController::scatter_preview_lines() const {
    std::vector<std::string> lines;
    if (!scatterActive_) return lines;
    const EditorDocument& document = workspace_.document();
    lines.push_back("Scatter Objects - preview (scene unchanged)");
    lines.push_back(std::string(scatterTargets_.size() == 1 ? "Surface: " : "Surfaces (" + std::to_string(scatterTargets_.size()) + "): ") +
                    scatter_names(document, scatterTargets_));
    std::string sources;
    for (const ScatterSource& source : scatter_sources()) sources += (sources.empty() ? "" : ", ") + source.label;
    lines.push_back("Copies of: " + sources + (scatter_sources_pinned() ? "  (set sources)" : ""));
    const auto meters = [](float value) {
        char buffer[32];
        std::snprintf(buffer, sizeof buffer, "%.2f m", static_cast<double>(value));
        return std::string(buffer);
    };
    lines.push_back("Placed " + std::to_string(scatterPlan_.samples.size()) + " of " +
                    std::to_string(scatterSettings_.count) + "   spacing " + meters(scatterSettings_.minSpacingMeters) +
                    "   seed " + std::to_string(scatterSettings_.seed));
    char variation[128];
    std::snprintf(variation, sizeof variation, "Align to surface: %s   turn +/-%.0f deg   scale %.2f-%.2f",
                  scatterSettings_.alignToSurface ? "on" : "off", static_cast<double>(scatterSettings_.yawJitterDegrees * 0.5F),
                  static_cast<double>(scatterSettings_.minScale), static_cast<double>(scatterSettings_.maxScale));
    lines.push_back(variation);
    if (!scatterPlan_.error.empty()) {
        lines.push_back("Cannot commit: " + scatterPlan_.error);
    } else if (scatterPlan_.samples.size() < scatterSettings_.count) {
        lines.push_back("Warning: room for " + std::to_string(scatterPlan_.samples.size()) + " (" +
                        std::to_string(scatterPlan_.tooClose) + " spots too close, " +
                        std::to_string(scatterPlan_.missedSurface) + " off the surface); lower spacing or count");
    }
    lines.push_back("Enter commit  Esc cancel  [ ] count  Shift+[ ] spacing  N seed  A align");
    lines.push_back("More settings in the Inspector");
    return lines;
}

bool NativeEditorController::scatter_settings_panel_active() const noexcept {
    return workspace_.mode() == EditorMode::Edit && !playSession_.active() &&
           (scatterActive_ || activeTool_ == EditorToolId::ScatterBrush);
}

std::vector<NativeEditorController::ScatterSettingRow> NativeEditorController::scatter_setting_rows() const {
    const ScatterSettings& v = scatterSettings_;
    const bool filling = scatterActive_;
    const bool brushing = activeTool_ == EditorToolId::ScatterBrush;
    const auto fixed = [](double value, int digits, std::string_view unit = {}) {
        char buffer[48];
        std::snprintf(buffer, sizeof buffer, "%.*f%s", digits, value, std::string(unit).c_str());
        return std::string(buffer);
    };
    return {
        {ScatterSettingField::Count, "Count", std::to_string(v.count), filling},
        {ScatterSettingField::Spacing, "Spacing", fixed(v.minSpacingMeters, 2, " m"), true},
        {ScatterSettingField::Seed, "Seed", std::to_string(v.seed), true},
        {ScatterSettingField::Align, "Align to surface", v.alignToSurface ? "on" : "off", true},
        {ScatterSettingField::YawJitter, "Random turn",
         v.yawJitterDegrees > 0.0F ? "+/-" + fixed(v.yawJitterDegrees * 0.5F, 0, " deg") : std::string("off"), true},
        {ScatterSettingField::MinScale, "Scale min", fixed(v.minScale, 2), true},
        {ScatterSettingField::MaxScale, "Scale max", fixed(v.maxScale, 2), true},
        {ScatterSettingField::BrushRadius, "Brush radius", fixed(v.brushRadiusMeters, 2, " m"), brushing},
        {ScatterSettingField::BrushDensity, "Brush density", fixed(v.brushDensity * 100.0F, 0, "%"), brushing},
    };
}

bool NativeEditorController::set_scatter_setting(ScatterSettingField field, std::string_view text, std::string* error) {
    const auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    std::string cleaned;
    for (const char c : text)
        if (!std::isspace(static_cast<unsigned char>(c)) && c != '%' && c != 'm' && c != '+' && c != '/') cleaned += c;
    if (cleaned.ends_with("deg")) cleaned.resize(cleaned.size() - 3U);
    ScatterSettings next = scatterSettings_;
    if (field == ScatterSettingField::Align) {
        if (cleaned == "on" || cleaned == "1" || cleaned == "true" || cleaned == "yes") next.alignToSurface = true;
        else if (cleaned == "off" || cleaned == "0" || cleaned == "false" || cleaned == "no") next.alignToSurface = false;
        else return fail("Align: type on or off");
    } else if (field == ScatterSettingField::Seed || field == ScatterSettingField::Count) {
        std::uint64_t value{};
        const auto [end, ec] = std::from_chars(cleaned.data(), cleaned.data() + cleaned.size(), value);
        if (ec != std::errc{} || end != cleaned.data() + cleaned.size() || cleaned.empty())
            return fail(std::string(field == ScatterSettingField::Seed ? "Seed" : "Count") + ": type a whole number");
        if (field == ScatterSettingField::Seed) next.seed = value;
        else next.count = static_cast<std::uint32_t>(std::min<std::uint64_t>(value, kMaxScatterCount));
    } else {
        char* end = nullptr;
        const float value = std::strtof(cleaned.c_str(), &end);
        if (cleaned.empty() || end != cleaned.c_str() + cleaned.size() || !std::isfinite(value))
            return fail("Type a number");
        switch (field) {
            case ScatterSettingField::Spacing: next.minSpacingMeters = value; break;
            case ScatterSettingField::YawJitter: next.yawJitterDegrees = value * 2.0F; break;  // typed as +/- half
            case ScatterSettingField::MinScale:
                next.minScale = value;
                next.maxScale = std::max(next.maxScale, value);
                break;
            case ScatterSettingField::MaxScale:
                next.maxScale = value;
                next.minScale = std::min(next.minScale, value);
                break;
            case ScatterSettingField::BrushRadius: next.brushRadiusMeters = value; break;
            case ScatterSettingField::BrushDensity: next.brushDensity = value / 100.0F; break;
            default: break;
        }
    }
    set_scatter_settings(next);
    return true;
}

void NativeEditorController::nudge_scatter_setting(ScatterSettingField field, int steps) {
    if (steps == 0) return;
    ScatterSettings next = scatterSettings_;
    const float n = static_cast<float>(steps);
    switch (field) {
        case ScatterSettingField::Count: {
            const std::int64_t count = static_cast<std::int64_t>(next.count) + steps * 5;
            next.count = static_cast<std::uint32_t>(std::clamp<std::int64_t>(count, 1, kMaxScatterCount));
            break;
        }
        case ScatterSettingField::Spacing: next.minSpacingMeters += 0.25F * n; break;
        case ScatterSettingField::Seed:
            next.seed = steps > 0 ? next.seed + static_cast<std::uint64_t>(steps)
                                  : next.seed - std::min<std::uint64_t>(next.seed, static_cast<std::uint64_t>(-steps));
            break;
        case ScatterSettingField::Align: next.alignToSurface = !next.alignToSurface; break;
        case ScatterSettingField::YawJitter: next.yawJitterDegrees += 30.0F * n; break;  // +/-15 deg per notch
        case ScatterSettingField::MinScale:
            next.minScale += 0.05F * n;
            next.maxScale = std::max(next.maxScale, next.minScale);
            break;
        case ScatterSettingField::MaxScale:
            next.maxScale += 0.05F * n;
            next.minScale = std::min(next.minScale, next.maxScale);
            break;
        case ScatterSettingField::BrushRadius: next.brushRadiusMeters *= std::pow(1.15F, n); break;
        case ScatterSettingField::BrushDensity: next.brushDensity += 0.05F * n; break;
    }
    set_scatter_settings(next);
}

bool NativeEditorController::handle_scatter_key(std::string_view normalized, bool control, bool shift, bool alt) {
    if (control || alt) return false;
    ScatterSettings settings = scatterSettings_;
    if (normalized == "escape") { cancel_scatter(); return true; }
    if (normalized == "return" || normalized == "enter" || normalized == "kp_enter") { (void)commit_scatter(); return true; }
    if (normalized == "bracketleft" || normalized == "[" || normalized == "bracketright" || normalized == "]") {
        const bool up = normalized == "bracketright" || normalized == "]";
        if (shift) settings.minSpacingMeters = std::max(kMinScatterSpacingMeters, settings.minSpacingMeters + (up ? 0.25F : -0.25F));
        else settings.count = up ? settings.count + 5U : (settings.count > 5U ? settings.count - 5U : 1U);
        set_scatter_settings(settings);
        return true;
    }
    if (normalized == "n") { ++settings.seed; set_scatter_settings(settings); return true; }
    if (normalized == "a") { settings.alignToSurface = !settings.alignToSurface; set_scatter_settings(settings); return true; }
    return false;
}

namespace {
// The scatter group an object was painted or filled into, if any (the nearest tagged ancestor).
std::optional<EditorObjectId> enclosing_scatter_group(const EditorDocument& document, EditorObjectId id) {
    const EditorObject* object = document.find_object(id);
    for (auto parent = object ? object->parent : std::nullopt; parent;) {
        const EditorObject* ancestor = document.find_object(*parent);
        if (!ancestor) break;
        if (is_scatter_group(*ancestor)) return ancestor->id;
        parent = ancestor->parent;
    }
    return std::nullopt;
}

// Where a scatter copy stands: the bottom centre of its world bounds.
Float3 scatter_copy_foot(const EditorObject& object) {
    const EditorObjectBounds bounds = object_world_bounds(object);
    return {(bounds.minimum.x + bounds.maximum.x) * 0.5F, bounds.minimum.y, (bounds.minimum.z + bounds.maximum.z) * 0.5F};
}

std::string scatter_group_name(std::size_t copies) { return "Scatter (" + std::to_string(copies) + ")"; }
bool has_generated_scatter_name(const EditorObject& group) { return group.name.starts_with("Scatter ("); }
} // namespace

void NativeEditorController::keep_isolated_with_subtree(EditorObjectId root) {
    // While View > Isolate Selection is on, objects outside the isolated set are hidden and
    // cannot be picked. Scatter output made under isolation joins the set, so a commit or a
    // stroke never adds copies the user cannot see (or erase).
    auto& isolated = viewportSettings_.isolatedObjects;
    if (isolated.empty()) return;
    for (const EditorObjectId id : collect_editor_object_subtree_ids(workspace_.document(), std::vector<EditorObjectId>{root}))
        isolated.insert(id);
}

void NativeEditorController::begin_scatter_brush_session() {
    floodFillArmed_ = false;  // another tool takes the next viewport click
    // Sources are the set scatter sources, or else what is selected when the tool is picked.
    // Picking the tool again starts a new session (and a new group), unless a scatter group is
    // the active selection: then strokes continue that group with its stored settings.
    brushSourceRoots_.clear();
    brushPrefab_.reset();
    brushGroup_.reset();
    brushPending_.clear();
    brushEraseIds_.clear();
    brushStroking_ = false;
    brushResizing_ = false;
    adopt_scatter_settings();
    const EditorDocument& document = workspace_.document();
    if (const auto active = workspace_.selected_object())
        if (const EditorObject* object = document.find_object(*active); object && is_scatter_group(*object))
            brushGroup_ = object->id;
    if (scatter_sources_pinned()) {
        for (const EditorObjectId id : pinnedScatterRoots_)
            if (document.find_object(id)) brushSourceRoots_.push_back(id);
        brushPrefab_ = pinnedScatterPrefab_;
    } else {
        const std::set<EditorObjectId> selected(workspace_.selected_objects().begin(), workspace_.selected_objects().end());
        brushSourceRoots_ = scatter_source_roots(document, selected);
        std::string error;
        brushPrefab_ = load_selected_scatter_prefab(&error);
        if (!error.empty()) set_status("Scatter brush: could not load the selected prefab: " + error, true, 6.0F);
    }
    recompute_layout();  // the inspector shows the scatter settings
    const std::size_t count = brush_sources().size();
    if (count == 0) {
        set_status("Scatter brush: select the objects to paint (or a prefab in Assets), then pick the tool again", true, 6.0F);
        return;
    }
    const EditorObject* group = brushGroup_ ? document.find_object(*brushGroup_) : nullptr;
    set_status("Scatter brush: painting " + std::to_string(count) + " source(s)" +
               (group ? " into " + group->name : std::string()) + "; Shift-drag erases, Ctrl+wheel sets the radius");
}

std::vector<ScatterSource> NativeEditorController::brush_sources() const {
    std::vector<ScatterSource> sources;
    for (const EditorObjectId root : brushSourceRoots_) {
        const EditorObject* object = workspace_.document().find_object(root);
        if (object) sources.push_back({&workspace_.document(), {root}, object->name});
    }
    if (brushPrefab_)
        sources.push_back({&brushPrefab_->templateDocument, brushPrefab_->rootTemplateObjectIds, "prefab " + brushPrefab_->name});
    return sources;
}

bool NativeEditorController::brush_ground_accepts(EditorObjectId id) const {
    const EditorDocument& document = workspace_.document();
    const EditorObject* object = document.find_object(id);
    if (!object || !object->flags.visible || !object->voxels || is_isolated_out(id)) return false;
    if (enclosing_scatter_group(document, id)) return false;  // never paint onto earlier copies
    for (auto cursor = std::optional<EditorObjectId>(id); cursor;) {
        if (std::find(brushSourceRoots_.begin(), brushSourceRoots_.end(), *cursor) != brushSourceRoots_.end())
            return false;  // nor onto the originals being copied
        const EditorObject* current = document.find_object(*cursor);
        cursor = current ? current->parent : std::nullopt;
    }
    return true;
}

void NativeEditorController::update_brush_cursor(int x, int y) {
    brushCursor_.reset();
    if (playSession_.active() || !layout_.viewport.contains(x, y)) return;
    const ViewportRay ray = make_viewport_ray(camera_, layout_.viewport, static_cast<float>(x), static_cast<float>(y));
    for (const EditorPickResult& hit : pick_editor_document_all(workspace_.document(), ray, 10000.0F,
                                                                &viewportSettings_.isolatedObjects)) {
        if (brush_ground_accepts(hit.objectId)) { brushCursor_ = hit.worldPosition; return; }
    }
}

void NativeEditorController::brush_dab() {
    if (!brushStroking_ || !brushCursor_) return;
    brushLastDab_ = brushCursor_;
    const EditorDocument& document = workspace_.document();
    const float radiusSquared = scatterSettings_.brushRadiusMeters * scatterSettings_.brushRadiusMeters;
    if (brushErasing_) {
        for (const auto& [id, object] : document.objects()) {
            if (!object.parent || object.flags.locked || is_isolated_out(id)) continue;
            const EditorObject* parent = document.find_object(*object.parent);
            if (!parent || !is_scatter_group(*parent)) continue;
            const Float3 foot = scatter_copy_foot(object);
            const float dx = foot.x - brushCursor_->x;
            const float dz = foot.z - brushCursor_->z;
            if (dx * dx + dz * dz <= radiusSquared && std::abs(foot.y - brushCursor_->y) <= scatterSettings_.brushRadiusMeters * 2.0F)
                brushEraseIds_.insert(id);
        }
        return;
    }
    const std::size_t sourceCount = brush_sources().size();
    if (sourceCount == 0) return;
    std::vector<Float3> occupied;
    for (const auto& [id, object] : document.objects()) {
        if (!object.parent) continue;
        const EditorObject* parent = document.find_object(*object.parent);
        if (parent && is_scatter_group(*parent)) occupied.push_back(scatter_copy_foot(object));
    }
    for (const ScatterSample& sample : brushPending_) occupied.push_back(sample.position);
    ScatterDabSettings settings;
    settings.minSpacingMeters = scatterSettings_.minSpacingMeters;
    settings.seed = scatterSettings_.seed * 0x9E3779B97F4A7C15ULL + (++brushDabCounter_);
    settings.sourceCount = sourceCount;
    settings.probeMeters = std::clamp(scatterSettings_.brushRadiusMeters * 0.1F, 0.1F, 1.0F);
    settings.density = scatterSettings_.brushDensity;
    settings.yawJitterDegrees = scatterSettings_.yawJitterDegrees;
    settings.minScale = scatterSettings_.minScale;
    settings.maxScale = scatterSettings_.maxScale;
    const auto ground = [this](EditorObjectId id) { return brush_ground_accepts(id); };
    // One stroke adds at most kMaxScatterCount copies (the fill's cap), however long or dense it is.
    if (brushPending_.size() >= kMaxScatterCount) {
        set_status("Scatter brush: this stroke reached " + std::to_string(kMaxScatterCount) +
                   " copies; release and paint again for more", true, 4.0F);
        return;
    }
    for (ScatterSample& sample : plan_scatter_dab(document, *brushCursor_, scatterSettings_.brushRadiusMeters, settings, occupied, ground)) {
        if (brushPending_.size() >= kMaxScatterCount) break;
        brushPending_.push_back(sample);
    }
}

void NativeEditorController::finish_brush_stroke(bool cancel) {
    const bool erasing = brushErasing_;
    brushStroking_ = false;
    brushErasing_ = false;
    brushLastDab_.reset();
    std::vector<ScatterSample> pending = std::move(brushPending_);
    std::set<EditorObjectId> eraseIds = std::move(brushEraseIds_);
    brushPending_.clear();
    brushEraseIds_.clear();
    if (cancel) { set_status("Scatter stroke cancelled; nothing changed"); return; }
    EditorDocument& document = workspace_.document();
    if (erasing) {
        if (eraseIds.empty()) return;
        // Remaining copy count per affected group, so generated "Scatter (N)" names stay true.
        std::map<EditorObjectId, std::size_t> removedPerGroup;
        for (const EditorObjectId id : eraseIds)
            if (const EditorObject* object = document.find_object(id); object && object->parent) ++removedPerGroup[*object->parent];
        auto command = std::make_unique<CompoundCommand>("Scatter brush erase");
        command->add(std::make_unique<RemoveObjectsCommand>(std::vector<EditorObjectId>(eraseIds.begin(), eraseIds.end()),
                                                            "Erase scatter copies"));
        for (const auto& [groupId, removed] : removedPerGroup) {
            const EditorObject* group = document.find_object(groupId);
            if (!group || !has_generated_scatter_name(*group)) continue;
            const std::size_t remaining = document.children_of(groupId).size() - removed;
            command->add(std::make_unique<RenameObjectCommand>(groupId, group->name, scatter_group_name(remaining)));
        }
        const CommandResult result = workspace_.commands().execute(document, std::move(command));
        if (!result.success) { set_status(result.message, true, 6.0F); return; }
        workspace_.prune_selection();
        recompute_layout();
        refresh_menu_state();
        set_status("Erased " + std::to_string(eraseIds.size()) + " scatter copies; Undo restores them");
        return;
    }
    if (pending.empty()) return;
    const std::vector<ScatterSource> sources = brush_sources();
    auto command = std::make_unique<CompoundCommand>("Scatter brush stroke");
    // One group per tool session; make a new one if Undo (or a delete) took the old one away.
    const EditorObject* group = brushGroup_ ? document.find_object(*brushGroup_) : nullptr;
    if (group && !is_scatter_group(*group)) group = nullptr;
    EditorObjectId groupId{};
    if (group) {
        groupId = group->id;
        if (has_generated_scatter_name(*group))
            command->add(std::make_unique<RenameObjectCommand>(
                groupId, group->name, scatter_group_name(document.children_of(groupId).size() + pending.size())));
        append_scatter_settings_update(*command, *group, scatterSettings_);  // the group remembers its settings
    } else {
        groupId = document.allocate_object_id();
        command->add(std::make_unique<AddObjectCommand>(
            make_scatter_group(groupId, pending.size(), pending.front().position, scatterSettings_), "Scatter group"));
    }
    const ScatterCopiesResult copies =
        append_scatter_copies(*command, document, groupId, pending, sources, scatterSettings_.alignToSurface);
    if (!copies.error.empty()) { set_status(copies.error, true, 6.0F); return; }
    const CommandResult result = workspace_.commands().execute(document, std::move(command));
    if (!result.success) { set_status(result.message, true, 6.0F); return; }
    brushGroup_ = groupId;
    keep_isolated_with_subtree(groupId);
    recompute_layout();
    refresh_menu_state();
    set_status("Painted " + std::to_string(pending.size()) + " copies into " +
               (document.find_object(groupId) ? document.find_object(groupId)->name : std::string("the scatter group")) +
               "; Undo removes this stroke");
}

NativeEditorController::ScatterBrushView NativeEditorController::scatter_brush_view() const {
    ScatterBrushView view;
    view.cursor = activeTool_ == EditorToolId::ScatterBrush ? brushCursor_ : std::nullopt;
    view.radius = scatterSettings_.brushRadiusMeters;
    view.stroking = brushStroking_;
    view.erasing = brushErasing_;
    view.pending = brushPending_;
    view.erasing_ids.assign(brushEraseIds_.begin(), brushEraseIds_.end());
    view.sourceCount = brush_sources().size();
    return view;
}

void NativeEditorController::set_scatter_brush_radius(float meters) {
    if (!std::isfinite(meters)) return;
    ScatterSettings settings = scatterSettings_;
    settings.brushRadiusMeters = meters;
    scatterSettings_ = sanitize_scatter_settings(settings);
    char text[64];
    std::snprintf(text, sizeof text, "Scatter brush radius %.2f m", static_cast<double>(scatterSettings_.brushRadiusMeters));
    set_status(text);
}

VoxelBooleanSelection NativeEditorController::voxel_boolean_selection() const {
    return evaluate_voxel_boolean_selection(workspace_.document(), workspace_.selected_objects(),
                                            workspace_.selected_object(), voxelBooleanPolicy_);
}

std::string NativeEditorController::voxel_boolean_disabled_reason() const {
    if (playSession_.active() || workspace_.mode() != EditorMode::Edit)
        return "Stop Play or Simulate (Shift+F5) before running a Boolean.";
    const VoxelBooleanSelection selection = voxel_boolean_selection();
    return selection.valid ? std::string{} : selection.reason;
}

bool NativeEditorController::begin_voxel_boolean(VoxelBooleanOperation operation) {
    floodFillArmed_ = false;  // another tool takes the next viewport click
    cancel_voxel_slice();
    if (voxelBoolean_.active()) {
        voxelBoolean_.set_operation(workspace_.document(), operation);
        refresh_menu_state();
        set_status("Boolean preview: " + std::string(voxel_boolean_operation_name(operation)), false, 4.0F);
        return true;
    }
    if (const std::string reason = voxel_boolean_disabled_reason(); !reason.empty()) {
        set_status(reason, true, 6.0F);
        return false;
    }
    cancel_scatter("Scatter preview closed: Boolean started");
    cancel_voxel_join("Join preview closed: Boolean started");
    cancel_text_edit();
    close_context_menu();
    if (gizmoDragging_) finish_gizmo_drag(true);
    finish_voxel_stroke();
    std::string error;
    if (!voxelBoolean_.begin(workspace_.document(), voxel_boolean_selection(), operation, voxelBooleanPolicy_, &error)) {
        set_status(error, true, 6.0F);
        return false;
    }
    refresh_menu_state();
    const VoxelBooleanResult& result = voxelBoolean_.result();
    set_status(result.can_commit()
                   ? "Boolean " + std::string(voxel_boolean_operation_name(operation)) +
                         " preview: Enter to commit, Esc to cancel"
                   : "Boolean preview: " + result.blocking_reason(),
               !result.can_commit(), 8.0F);
    return true;
}

CommandResult NativeEditorController::commit_voxel_boolean() {
    if (!voxelBoolean_.active()) {
        const std::string reason = voxelBoolean_.commit_blocked_reason();
        set_status(reason, true, 6.0F);
        return CommandResult::fail(reason);
    }
    voxelBoolean_.set_operand_policy(voxelBooleanPolicy_);
    const auto beforeSelection = capture_selection_snapshot();
    const CommandResult result = voxelBoolean_.commit(workspace_);
    if (!result.success) {
        set_status("Boolean not applied: " + result.message, true, 8.0F);
        refresh_menu_state();
        return result;
    }
    workspace_.commands().set_last_selection_transition(beforeSelection, capture_selection_snapshot());
    const auto& report = *voxelBoolean_.last_commit();
    recompute_layout();
    refresh_menu_state();
    std::string text = "Boolean " + std::string(voxel_boolean_operation_name(report.operation)) + " applied: +" +
                       std::to_string(report.stats.addedVoxels) + " / -" + std::to_string(report.stats.removedVoxels) +
                       " voxels, " + (report.collisionEnabled ? "collision rebuilt (" + std::to_string(report.collisionBoxes) + " boxes)"
                                                               : std::string("collision off on target"));
    if (report.policy == VoxelBooleanOperandPolicy::Hide) text += ", operands hidden";
    else if (report.policy == VoxelBooleanOperandPolicy::Delete) text += ", operands deleted";
    set_status(text + ". Ctrl+Z undoes it in one step.", false, 8.0F);
    return result;
}

void NativeEditorController::cancel_voxel_boolean(std::string_view reason) {
    if (!voxelBoolean_.active()) return;
    voxelBoolean_.cancel();
    refresh_menu_state();
    set_status(reason.empty() ? std::string("Boolean preview cancelled; the scene is unchanged") : std::string(reason),
               false, 4.0F);
}

bool NativeEditorController::swap_voxel_boolean_target() {
    if (!voxelBoolean_.active() || voxelBoolean_.operands().empty()) {
        set_status("No Boolean preview is open", true);
        return false;
    }
    const EditorObjectId newTarget = voxelBoolean_.operands().front();
    const VoxelBooleanOperation operation = voxelBoolean_.operation();
    voxelBoolean_.cancel();
    workspace_.add_to_selection(newTarget); // makes it the active (primary) selection
    if (!begin_voxel_boolean(operation)) return false;
    const EditorObject* target = workspace_.document().find_object(newTarget);
    set_status("Boolean target is now " + (target ? "'" + target->name + "'" : std::string("the first operand")),
               false, 4.0F);
    return true;
}

void NativeEditorController::set_voxel_boolean_operand_policy(VoxelBooleanOperandPolicy policy) {
    voxelBooleanPolicy_ = policy;
    voxelBoolean_.set_operand_policy(policy);
    refresh_menu_state();
    set_status("Boolean operands after commit: " + std::string(voxel_boolean_operand_policy_name(policy)), false, 4.0F);
}

bool NativeEditorController::handle_voxel_boolean_key(std::string_view normalized, bool control, bool shift, bool alt) {
    (void)shift;
    if (control || alt) return false;
    if (normalized == "escape") { cancel_voxel_boolean(); return true; }
    if (normalized == "return" || normalized == "enter" || normalized == "kp_enter") {
        (void)commit_voxel_boolean();
        return true;
    }
    if (normalized == "1" || normalized == "2" || normalized == "3") {
        (void)begin_voxel_boolean(normalized == "1" ? VoxelBooleanOperation::Union
                                  : normalized == "2" ? VoxelBooleanOperation::Difference
                                                      : VoxelBooleanOperation::Intersection);
        return true;
    }
    if (normalized == "s") { (void)swap_voxel_boolean_target(); return true; }
    if (normalized == "o") {
        set_voxel_boolean_operand_policy(voxelBooleanPolicy_ == VoxelBooleanOperandPolicy::Hide ? VoxelBooleanOperandPolicy::Delete
                                         : voxelBooleanPolicy_ == VoxelBooleanOperandPolicy::Delete ? VoxelBooleanOperandPolicy::Keep
                                                                                                   : VoxelBooleanOperandPolicy::Hide);
        return true;
    }
    if (normalized == "m") {
        voxelBoolean_.set_overlap_material(workspace_.document(),
            voxelBoolean_.overlap_material() == VoxelBooleanOverlapMaterial::KeepPrimary
                ? VoxelBooleanOverlapMaterial::TakeOperand : VoxelBooleanOverlapMaterial::KeepPrimary);
        refresh_menu_state();
        set_status(voxelBoolean_.overlap_material() == VoxelBooleanOverlapMaterial::KeepPrimary
                       ? "Overlapping voxels keep the target's material"
                       : "Overlapping voxels take the operand's material", false, 4.0F);
        return true;
    }
    return false;
}

void NativeEditorController::voxel_boolean_tick() {
    if (voxelSlice_.active() && (workspace_.selection_count() != 1 || workspace_.selected_object() != voxelSlice_.target() ||
        !voxelSlice_.target_unchanged(workspace_.document()))) {
        cancel_voxel_slice();
        set_status("Slice preview cancelled: selection or target changed", true);
    }
    if (voxelBoolean_.active()) {
        std::set<EditorObjectId> participants(voxelBoolean_.operands().begin(), voxelBoolean_.operands().end());
        participants.insert(voxelBoolean_.target());
        if (participants != workspace_.selected_objects() || workspace_.selected_object() != voxelBoolean_.target()) {
            cancel_voxel_boolean("Boolean preview cancelled: the selection changed");
        } else if (!voxelBoolean_.refresh(workspace_.document())) {
            refresh_menu_state();
            set_status("Boolean preview closed: an object it used was removed or changed type", true, 6.0F);
        }
    }
    if (voxelJoin_.active()) {
        const std::vector<EditorObjectId> participants = voxelJoin_.participants();
        if (std::set<EditorObjectId>(participants.begin(), participants.end()) != workspace_.selected_objects()) {
            cancel_voxel_join("Join preview cancelled: the selection changed");
        } else if (!voxelJoin_.refresh(workspace_.document())) {
            refresh_menu_state();
            set_status("Join preview closed: an object it used was removed or changed type", true, 6.0F);
        }
    }
    // Keep the Boolean commands' enabled state and disabled reasons in step with the selection.
    std::uint64_t key = editor_selection_fingerprint(workspace_.selected_objects());
    key = key * 1099511628211ULL ^ workspace_.selected_object().value_or(0U);
    key = key * 1099511628211ULL ^ workspace_.commands().size();
    key = key * 1099511628211ULL ^ (workspace_.commands().can_undo() ? 1U : 0U) ^ (workspace_.commands().can_redo() ? 2U : 0U);
    key = key * 1099511628211ULL ^ (playSession_.active() ? 1U : 0U) ^ (voxelBoolean_.active() ? 2U : 0U) ^
          (voxelSlice_.active() ? 4U : 0U) ^ (voxelJoin_.active() ? 8U : 0U);
    // File > Import Model depends on the Assets-panel selection.
    key = key * 1099511628211ULL ^ std::hash<std::string>{}(assetBrowserState_.selectedId.value_or(std::string()));
    if (key != voxelBooleanMenuKey_) {
        voxelBooleanMenuKey_ = key;
        refresh_menu_state();
    }
}

std::vector<std::string> NativeEditorController::voxel_boolean_preview_lines() const {
    return voxelBoolean_.describe(workspace_.document());
}

std::vector<VoxelBooleanPreviewMarker> NativeEditorController::voxel_boolean_preview_markers(std::size_t limit) const {
    std::vector<VoxelBooleanPreviewMarker> markers;
    if (!voxelBoolean_.active()) return markers;
    const EditorObject* target = workspace_.document().find_object(voxelBoolean_.target());
    if (!target) return markers;
    const UiRect viewport = layout_.viewport;
    const float viewportHeight = static_cast<float>(std::max(1, viewport.height));
    const float tangent = std::tan(camera_.verticalFovRadians * 0.5F);
    const float size = target->voxelSizeMeters;
    const auto push = [&](Int3 voxel, VoxelBooleanPreviewMarker::Kind kind) {
        if (markers.size() >= limit) return;
        const Float3 local{(static_cast<float>(voxel.x) + 0.5F) * size, (static_cast<float>(voxel.y) + 0.5F) * size,
                           (static_cast<float>(voxel.z) + 0.5F) * size};
        const ScreenPoint screen = project_world_to_screen(camera_, viewport, transform_point(target->transform, multiply_components(local,target->voxelScale)));
        if (!screen.visible) return;
        const float radius = camera_.projection == EditorProjection::Perspective
            ? size * viewportHeight / std::max(0.001F, 2.0F * screen.depth * tangent)
            : size * viewportHeight / std::max(0.001F, camera_.orthographicHeight);
        markers.push_back({kind, screen.x, screen.y, screen.depth, std::clamp(radius * 0.72F, 1.5F, 12.0F)});
    };
    const VoxelBooleanResult& result = voxelBoolean_.result();
    for (const VoxelBooleanChange& change : result.changes)
        push(change.voxel, change.after == kAirMaterial ? VoxelBooleanPreviewMarker::Kind::Removed
                           : change.before == kAirMaterial ? VoxelBooleanPreviewMarker::Kind::Added
                                                           : VoxelBooleanPreviewMarker::Kind::Repainted);
    if (result.operation == VoxelBooleanOperation::Union)
        for (const Int3 cell : result.overlapCells) push(cell, VoxelBooleanPreviewMarker::Kind::Overlap);
    std::sort(markers.begin(), markers.end(), [](const VoxelBooleanPreviewMarker& a, const VoxelBooleanPreviewMarker& b) {
        return a.depth > b.depth;
    });
    return markers;
}

} // namespace dve::editor

namespace dve::editor {
std::string NativeEditorController::voxel_slice_disabled_reason() const {
    if (playSession_.active() || workspace_.mode() != EditorMode::Edit)
        return "Stop Play or Simulate before slicing.";
    if (workspace_.selection_count() != 1)
        return "Select exactly one occupied voxel object to slice.";
    const auto* o = workspace_.document().find_object(*workspace_.selected_object());
    if (!o || !o->voxels || o->text3d || o->gaborVolume || !o->voxels->occupied_voxel_count())
        return "Select an occupied voxel object.";
    if (o->flags.locked)
        return "Unlock the object before slicing.";
    return {};
}
// --- Join (ART-062) and Separate Islands (ART-061) --------------------------------------------

std::string NativeEditorController::voxel_join_disabled_reason() const {
    if (playSession_.active() || workspace_.mode() != EditorMode::Edit) return "Stop Play or Simulate before joining.";
    std::string problem = EditorVoxelJoinSession::selection_problem(workspace_.document(), workspace_.selected_objects(),
                                                                    workspace_.selected_object());
    if (!problem.empty()) return problem;
    // Under View > Isolate Selection, objects isolated out of view cannot be seen: never merge them.
    for (const EditorObjectId id : workspace_.selected_objects())
        if (is_isolated_out(id)) {
            const EditorObject* object = workspace_.document().find_object(id);
            return "'" + (object ? object->name : std::to_string(id)) +
                   "' is isolated out of view. Turn isolation off or deselect it.";
        }
    return {};
}

bool NativeEditorController::begin_voxel_join() {
    floodFillArmed_ = false;  // another tool takes the next viewport click
    if (const std::string reason = voxel_join_disabled_reason(); !reason.empty()) {
        set_status(reason, true, 6.0F);
        return false;
    }
    // Join shares the preview panel slot with Scatter, Slice and Boolean: one at a time.
    cancel_voxel_slice();
    if (voxelBoolean_.active()) cancel_voxel_boolean("Boolean preview closed: Join started");
    cancel_scatter("Scatter preview closed: Join started");
    cancel_text_edit();
    close_context_menu();
    if (gizmoDragging_) finish_gizmo_drag(true);
    finish_voxel_stroke();
    std::string error;
    if (!voxelJoin_.begin(workspace_.document(), workspace_.selected_objects(), workspace_.selected_object(), &error)) {
        set_status(error, true, 6.0F);
        return false;
    }
    refresh_menu_state();
    set_status(voxelJoin_.needs_choice()
                   ? "Join preview: some objects are off the target's grid. R resample, G group instead, Tab next target"
                   : "Join preview: Enter joins, Esc cancels, Tab picks another target",
               voxelJoin_.needs_choice(), 8.0F);
    return true;
}

CommandResult NativeEditorController::commit_voxel_join() {
    if (!voxelJoin_.active()) {
        set_status("No Join preview is open", true);
        return CommandResult::fail("No Join preview is open");
    }
    const std::size_t count = voxelJoin_.operands().size() + 1U;
    const EditorObjectId target = voxelJoin_.target();
    const auto beforeSelection = capture_selection_snapshot();
    const CommandResult result = voxelJoin_.commit(workspace_);
    if (!result.success) {
        set_status(result.message, true, 6.0F);
        return result;
    }
    workspace_.commands().set_last_selection_transition(beforeSelection, capture_selection_snapshot());
    recompute_layout();
    refresh_menu_state();
    const EditorObject* joined = workspace_.document().find_object(target);
    // Rebuild the target's collision proxy now (as Boolean does) so its cost is known at commit.
    std::string collision = "collision off on target";
    if (joined && joined->flags.collisionEnabled && joined->voxels)
        collision = "collision rebuilt (" + std::to_string(build_merged_object_box_proxy(*joined->voxels).size()) + " boxes)";
    set_status("Joined " + std::to_string(count) + " objects into " + (joined ? joined->name : std::string("the target")) +
               ", " + collision + "; Undo restores them");
    return result;
}

void NativeEditorController::cancel_voxel_join(std::string reason) {
    if (!voxelJoin_.active()) return;
    voxelJoin_.cancel();
    refresh_menu_state();
    set_status(reason.empty() ? "Join cancelled; scene unchanged" : std::move(reason));
}

bool NativeEditorController::handle_voxel_join_key(std::string_view normalized, bool control, bool shift, bool alt) {
    (void)shift;
    if (control || alt) return false;
    if (normalized == "escape") { cancel_voxel_join(); return true; }
    if (normalized == "return" || normalized == "enter" || normalized == "kp_enter") {
        (void)commit_voxel_join();
        return true;
    }
    if (normalized == "r" && voxelJoin_.has_mismatch()) {
        voxelJoin_.choose_resample();
        refresh_menu_state();
        set_status("Join: off-grid objects will be resampled into the target's grid; Enter joins", false, 6.0F);
        return true;
    }
    if (normalized == "g") {
        // Group instead: keep every object as it is and put them under one group.
        cancel_voxel_join("Join cancelled: grouping instead");
        (void)dispatch_action("edit.group");
        return true;
    }
    if (normalized == "tab") {
        voxelJoin_.cycle_target(workspace_.document());
        workspace_.add_to_selection(voxelJoin_.target());  // already selected: makes it the active object
        refresh_menu_state();
        const EditorObject* target = workspace_.document().find_object(voxelJoin_.target());
        set_status("Join target: " + (target ? target->name : std::string("?")), false, 4.0F);
        return true;
    }
    return false;
}

std::string NativeEditorController::separate_islands_disabled_reason() const {
    if (playSession_.active() || workspace_.mode() != EditorMode::Edit) return "Stop Play or Simulate before separating.";
    if (workspace_.selection_count() == 0) return "Select a voxel object to split into its connected pieces.";
    std::string last;
    for (const EditorObjectId id : workspace_.selected_objects()) {
        last = separate_islands_problem(workspace_.document(), id);
        if (last.empty()) return {};
    }
    return last;
}

bool NativeEditorController::separate_voxel_islands() {
    if (const std::string reason = separate_islands_disabled_reason(); !reason.empty()) {
        set_status(reason, true, 6.0F);
        return false;
    }
    cancel_voxel_join("Join preview closed: Separate Islands");
    if (voxelBoolean_.active()) cancel_voxel_boolean("Boolean preview closed: Separate Islands");
    cancel_voxel_slice();
    const std::vector<EditorObjectId> ids(workspace_.selected_objects().begin(), workspace_.selected_objects().end());
    SeparateIslandsBuild build = build_separate_islands_command(workspace_.document(), ids);
    if (!build.command) {
        set_status(build.skipped.empty() ? "Nothing to separate" : "Nothing to separate: " + build.skipped.front(), true, 6.0F);
        return false;
    }
    const auto beforeSelection = capture_selection_snapshot();
    const CommandResult result = workspace_.commands().execute(workspace_.document(), std::move(build.command));
    if (!result.success) {
        set_status(result.message, true, 6.0F);
        return false;
    }
    // Select every piece; the originals (largest pieces) stay selected and active.
    const auto active = workspace_.selected_object();
    for (std::size_t i = 0; i < build.newObjects.size(); ++i) {
        const EditorObjectId id = build.newObjects[i];
        workspace_.add_to_selection(id);
        // Under isolation, a piece stays visible exactly when its original was.
        if (i < build.newObjectSources.size() && viewportSettings_.isolatedObjects.contains(build.newObjectSources[i]))
            viewportSettings_.isolatedObjects.insert(id);
    }
    if (active) workspace_.add_to_selection(*active);
    prune_voxel_selection();
    workspace_.commands().set_last_selection_transition(beforeSelection, capture_selection_snapshot());
    recompute_layout();
    refresh_menu_state();
    std::string message = "Separated " + std::to_string(build.newObjects.size()) + " piece(s) from " +
                          std::to_string(build.separatedObjects) +
                          " object(s); the largest piece keeps each original. Undo restores";
    if (!build.skipped.empty())
        message += ". Skipped " + std::to_string(build.skipped.size()) + ": " + build.skipped.front();
    set_status(message, !build.skipped.empty(), build.skipped.empty() ? 4.0F : 8.0F);
    return true;
}

// --- Long job state (see start_long_job) -------------------------------------------------------

namespace {
// Changes to the scene while a long job runs: the command history plus each object's identity,
// name, parent, voxel revision, transform, flags and components. EditorDocument::revision() is
// the saved revision, so it cannot tell.
std::uint64_t scene_change_fingerprint(const EditorDocument& document, const EditorCommandStack& commands) {
    std::uint64_t h = 1469598103934665603ULL;
    const auto mix = [&](std::uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6U) + (h >> 2U); h *= 1099511628211ULL; };
    const auto mixf = [&](float v) { mix(std::bit_cast<std::uint32_t>(v)); };
    mix(commands.size());
    mix((commands.can_undo() ? 1U : 0U) | (commands.can_redo() ? 2U : 0U));
    mix(std::hash<std::string_view>{}(commands.undo_label()));
    mix(document.objects().size());
    for (const auto& [id, object] : document.objects()) {
        mix(id);
        mix(object.parent.value_or(0));
        mix(std::hash<std::string>{}(object.name));
        mix(object.polygon?object.polygon->contentHash:0);
        mixf(object.voxelScale.x);mixf(object.voxelScale.y);mixf(object.voxelScale.z);
        mix(object.voxels ? object.voxels->revision() : 0U);
        mix(static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(object.voxels.get())));
        mixf(object.transform.position.x); mixf(object.transform.position.y); mixf(object.transform.position.z);
        mixf(object.transform.rotation.x); mixf(object.transform.rotation.y); mixf(object.transform.rotation.z);
        mixf(object.transform.rotation.w);
        mix((object.flags.visible ? 1U : 0U) | (object.flags.locked ? 2U : 0U) | (object.flags.anchored ? 4U : 0U) |
            (object.flags.structural ? 8U : 0U) | (object.flags.collisionEnabled ? 16U : 0U) | (object.flags.decorative ? 32U : 0U));
        mix(object.components.size());
    }
    return h;
}
} // namespace

struct NativeEditorController::LongJob {
    LongJobKind kind{LongJobKind::Idle};
    std::string label;
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
    std::atomic<float> progress{0.0F};
    mutable std::mutex mutex;
    std::string phase;
    std::string error;
    std::uint64_t documentRevision{};
    std::function<bool(LongJob&)> commit;
    void report(float value, std::string text) {
        progress.store(std::clamp(value, 0.0F, 1.0F));
        std::lock_guard lock(mutex);
        phase = std::move(text);
    }
    [[nodiscard]] bool cancelled() const noexcept { return cancel.load(); }
};

NativeEditorController::LongJobCancelOnExit::~LongJobCancelOnExit() {
    if (job && *job) (*job)->cancel = true;
}

// --- Flood Fill, Drop to Surface, collision, validation (honest commands) --------------------

namespace {
constexpr std::size_t kMaxFloodFillVoxels = 2'000'000;
}

bool NativeEditorController::arm_flood_fill() {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) {
        set_status("Stop Play or Simulate before filling", true);
        return false;
    }
    if (materials_.find(activeMaterial_) == nullptr) {
        set_status("Pick a material first (sample one with Mouse4 or the material list)", true);
        return false;
    }
    // The next click belongs to the fill: close any Join/Slice/Boolean/Scatter preview first
    // (one preview at a time, like Join), so the fill cannot edit an object under a preview.
    cancel_voxel_slice();
    if (voxelBoolean_.active()) cancel_voxel_boolean("Boolean preview closed: Flood Fill armed");
    cancel_scatter("Scatter preview closed: Flood Fill armed");
    cancel_voxel_join("Join preview closed: Flood Fill armed");
    floodFillArmed_ = true;
    set_status("Flood Fill: click a voxel to repaint its connected same-material region with material " +
               std::to_string(activeMaterial_) + " (Esc cancels)", false, 10.0F);
    return true;
}

bool NativeEditorController::flood_fill_at(EditorObjectId objectId, Int3 start) {
    floodFillArmed_ = false;
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) {
        set_status("Stop Play or Simulate before filling", true);
        return false;
    }
    if (longJob_) { set_status(long_job_busy_reason(), true); return false; }
    const EditorObject* object = workspace_.document().find_object(objectId);
    if (object && (!object->flags.visible || is_isolated_out(objectId))) {
        set_status("Flood Fill: " + object->name + " is hidden or isolated out of view", true);
        return false;
    }
    if (!object || !object->voxels || object->polygon || object->text3d || object->gaborVolume) {
        set_status("Flood Fill works on voxel objects", true);
        return false;
    }
    if (object->flags.locked) { set_status("Unlock " + object->name + " before filling", true); return false; }
    const MaterialId from = object->voxels->material_at(start);
    if (from == kAirMaterial) { set_status("Flood Fill: that cell is empty", true); return false; }
    if (from == activeMaterial_) {
        set_status("Flood Fill: that region already uses material " + std::to_string(activeMaterial_), true);
        return false;
    }
    prune_voxel_selection();
    const bool limitToSelection = !voxelSelection_.empty() && objectId == voxelSelectionObject_;
    if (limitToSelection && !voxelSelection_.contains(start)) {
        set_status("Flood Fill: click inside the voxel selection (or clear it)", true);
        return false;
    }
    // The fill reads a snapshot (a copy of the object and of the voxel selection), so a long
    // fill can run on the worker; the repaint is applied on the UI thread as one undo step.
    const std::uint64_t occupied = object->voxels->occupied_voxel_count();
    const bool background = occupied > longJobThreshold_;
    std::shared_ptr<const VoxelObject> voxels = background ? std::shared_ptr<const VoxelObject>(clone_voxel_object(*object->voxels))
                                                           : std::shared_ptr<const VoxelObject>(object->voxels.get(), [](const VoxelObject*) {});
    auto selection = limitToSelection ? std::make_shared<const std::set<Int3>>(voxelSelection_) : nullptr;
    const MaterialId to = activeMaterial_;
    const std::uint64_t revision = object->voxels->revision();
    struct FillResult { std::vector<VoxelBooleanChange> changes; bool tooLarge{}; };
    auto result = std::make_shared<FillResult>();
    return start_long_job(
        LongJobKind::FloodFill, "Flood Fill " + object->name, occupied,
        [voxels, selection, start, from, to, occupied, result](LongJob& job) {
            std::set<Int3> seen{start};
            std::vector<Int3> stack{start};
            static constexpr Int3 kSteps[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            while (!stack.empty()) {
                const Int3 at = stack.back();
                stack.pop_back();
                result->changes.push_back({at, from, to});
                if (result->changes.size() > kMaxFloodFillVoxels) { result->tooLarge = true; return; }
                if ((result->changes.size() & 0x3FFFU) == 0U) {
                    if (job.cancelled()) return;
                    job.report(0.9F * static_cast<float>(result->changes.size()) / static_cast<float>(std::max<std::uint64_t>(1, occupied)),
                               "Filling: " + std::to_string(result->changes.size()) + " voxels so far");
                }
                for (const Int3 step : kSteps) {
                    const Int3 next{at.x + step.x, at.y + step.y, at.z + step.z};
                    if (seen.contains(next) || voxels->material_at(next) != from) continue;
                    if (selection && !selection->contains(next)) continue;
                    seen.insert(next);
                    stack.push_back(next);
                }
            }
            job.report(0.95F, "Sorting " + std::to_string(result->changes.size()) + " voxels");
            std::sort(result->changes.begin(), result->changes.end());
        },
        [this, objectId, revision, to, result](LongJob&) {
            if (result->tooLarge) {
                set_status("Flood Fill stopped: the region has more than 2,000,000 voxels", true, 6.0F);
                return false;
            }
            const EditorObject* target = workspace_.document().find_object(objectId);
            if (!target || !target->voxels || target->voxels->revision() != revision) {
                set_status("Flood Fill was not applied: the object changed", true, 6.0F);
                return false;
            }
            const std::vector<Int3> anchors(target->anchors.begin(), target->anchors.end());
            const std::size_t count = result->changes.size();
            const CommandResult executed = workspace_.commands().execute(
                workspace_.document(),
                std::make_unique<ApplyVoxelBooleanCommand>(objectId, std::move(result->changes), anchors, anchors, "Flood fill"));
            if (!executed.success) { set_status(executed.message, true, 6.0F); return false; }
            set_status("Filled " + std::to_string(count) + " voxels with material " + std::to_string(to) + "; Undo restores");
            return true;
        });
}

bool NativeEditorController::drop_selection_to_surface() {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) {
        set_status("Stop Play or Simulate before moving objects", true);
        return false;
    }
    const EditorDocument& document = workspace_.document();
    const std::set<EditorObjectId>& selected = workspace_.selected_objects();
    if (selected.empty()) { set_status("Select the objects to drop first", true); return false; }
    // Roots: selected objects without a selected ancestor. Each drops as a unit with its subtree.
    std::vector<EditorObjectId> roots;
    for (const EditorObjectId id : selected) {
        const EditorObject* object = document.find_object(id);
        if (!object) continue;
        if (object->flags.locked) { set_status("Unlock " + object->name + " before dropping it", true); return false; }
        // Same rule as Join and Scatter (#99/#100): never move what cannot be seen.
        if (!object->flags.visible) { set_status("'" + object->name + "' is hidden. Show it or deselect it before dropping", true); return false; }
        if (is_isolated_out(id)) {
            set_status("'" + object->name + "' is isolated out of view. Turn isolation off or deselect it", true);
            return false;
        }
        bool nested = false;
        for (auto parent = object->parent; parent && !nested;) {
            nested = selected.contains(*parent);
            const EditorObject* ancestor = document.find_object(*parent);
            parent = ancestor ? ancestor->parent : std::nullopt;
        }
        if (!nested) roots.push_back(id);
    }
    const std::vector<EditorObjectId> moving = collect_editor_object_subtree_ids(document, roots);
    const std::set<EditorObjectId> movingSet(moving.begin(), moving.end());
    std::vector<ObjectTransformChange> changes;
    std::size_t dropped = 0, resting = 0, unsupported = 0;
    for (const EditorObjectId root : roots) {
        const std::vector<EditorObjectId> subtree = collect_editor_object_subtree_ids(document, std::vector<EditorObjectId>{root});
        EditorObjectBounds bounds;
        for (const EditorObjectId id : subtree) {
            const EditorObject* object = document.find_object(id);
            if (!object) continue;
            const EditorObjectBounds b = object_world_bounds(*object);
            if (!b.valid) continue;
            if (!bounds.valid) { bounds = b; continue; }
            bounds.minimum = {std::min(bounds.minimum.x, b.minimum.x), std::min(bounds.minimum.y, b.minimum.y), std::min(bounds.minimum.z, b.minimum.z)};
            bounds.maximum = {std::max(bounds.maximum.x, b.maximum.x), std::max(bounds.maximum.y, b.maximum.y), std::max(bounds.maximum.z, b.maximum.z)};
        }
        if (!bounds.valid) { ++unsupported; continue; }
        // Sample a 5x5 grid over the footprint; the highest surface below the bottom wins.
        std::optional<float> surface;
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 5; ++j) {
                const float x = bounds.minimum.x + (bounds.maximum.x - bounds.minimum.x) * (0.05F + 0.225F * static_cast<float>(i));
                const float z = bounds.minimum.z + (bounds.maximum.z - bounds.minimum.z) * (0.05F + 0.225F * static_cast<float>(j));
                const ViewportRay down{{x, bounds.minimum.y + 1.0e-3F, z}, {0.0F, -1.0F, 0.0F}};
                for (const EditorPickResult& hit : pick_editor_document_all(document, down, 100000.0F,
                                                                            &viewportSettings_.isolatedObjects)) {
                    const EditorObject* other = document.find_object(hit.objectId);
                    if (!other || movingSet.contains(hit.objectId) || !other->flags.visible) continue;
                    if (!surface || hit.worldPosition.y > *surface) surface = hit.worldPosition.y;
                    break;
                }
            }
        if (!surface && bounds.minimum.y > 1.0e-4F) surface = 0.0F;  // nothing below: the grid floor
        if (!surface) { ++unsupported; continue; }
        const float delta = *surface - bounds.minimum.y;
        if (std::abs(delta) < 1.0e-4F) { ++resting; continue; }
        for (const EditorObjectId id : subtree) {
            const EditorObject* object = document.find_object(id);
            // Attached children follow their parent; only authored (unattached) transforms move.
            if (!object || (id != root && object->attachment)) continue;
            RigidTransform after = object->transform;
            after.position.y += delta;
            changes.push_back({id, object->transform, after});
        }
        ++dropped;
    }
    if (changes.empty()) {
        set_status(resting > 0 ? "Already resting on the surface below" : "Nothing below to drop onto", resting == 0);
        return false;
    }
    const CommandResult result = workspace_.commands().execute(
        workspace_.document(), std::make_unique<TransformObjectsCommand>(std::move(changes), "Drop to surface"));
    if (!result.success) { set_status(result.message, true, 6.0F); return false; }
    std::string text = "Dropped " + std::to_string(dropped) + " object(s) onto the surface below";
    if (resting > 0) text += "; " + std::to_string(resting) + " already resting";
    if (unsupported > 0) text += "; " + std::to_string(unsupported) + " with nothing below";
    set_status(text);
    return true;
}

const std::vector<VoxelBox>& NativeEditorController::collision_proxy(EditorObjectId id) const {
    static const std::vector<VoxelBox> kNone;
    const EditorObject* object = workspace_.document().find_object(id);
    if (!object || !object->voxels || object->polygon || object->text3d || object->gaborVolume || !object->flags.collisionEnabled) {
        collisionProxyCache_.erase(id);
        return kNone;
    }
    const std::uint64_t revision = object->voxels->revision();
    const auto stamp = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(object->voxels.get()));
    auto [it, inserted] = collisionProxyCache_.try_emplace(id);
    CachedCollisionProxy& cached = it->second;
    if (inserted || cached.revision != revision || cached.objectStamp != stamp) {
        cached.revision = revision;
        cached.objectStamp = stamp;
        cached.boxes = build_merged_object_box_proxy(*object->voxels);
    }
    return cached.boxes;
}

bool NativeEditorController::rebuild_collision_proxies() {
    if (longJob_) { set_status(long_job_busy_reason(), true); return false; }
    const EditorDocument& document = workspace_.document();
    std::vector<EditorObjectId> ids;
    if (!workspace_.selected_objects().empty()) ids.assign(workspace_.selected_objects().begin(), workspace_.selected_objects().end());
    else for (const auto& [id, object] : document.objects()) ids.push_back(id);
    struct Target {
        EditorObjectId id{};
        std::uint64_t revision{};
        std::uint64_t stamp{};
        std::shared_ptr<const VoxelObject> voxels;
        std::vector<VoxelBox> boxes;
    };
    std::vector<Target> targets;
    std::size_t disabled = 0;
    std::uint64_t voxelTotal = 0;
    for (const EditorObjectId id : ids) {
        const EditorObject* object = document.find_object(id);
        if (!object || !object->voxels || object->polygon || object->text3d || object->gaborVolume || object->voxels->occupied_voxel_count() == 0) continue;
        if (!object->flags.collisionEnabled) { ++disabled; continue; }
        voxelTotal += object->voxels->occupied_voxel_count();
        targets.push_back({id, object->voxels->revision(), static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(object->voxels.get())), nullptr, {}});
    }
    if (targets.empty()) {
        set_status(disabled > 0 ? "The selected objects have Collision turned off" : "No voxel objects to rebuild collision for", true);
        return false;
    }
    // Snapshot the voxels when the work goes to the worker; small work reads them in place.
    const bool background = voxelTotal > longJobThreshold_;
    for (Target& target : targets) {
        const VoxelObject& live = *document.find_object(target.id)->voxels;
        target.voxels = background ? std::shared_ptr<const VoxelObject>(clone_voxel_object(live))
                                   : std::shared_ptr<const VoxelObject>(&live, [](const VoxelObject*) {});
    }
    auto shared = std::make_shared<std::vector<Target>>(std::move(targets));
    return start_long_job(
        LongJobKind::RebuildCollision, "Rebuild Collision", voxelTotal,
        [shared](LongJob& job) {
            for (std::size_t i = 0; i < shared->size(); ++i) {
                if (job.cancelled()) return;
                job.report(static_cast<float>(i) / static_cast<float>(shared->size()),
                           "Object " + std::to_string(i + 1) + " of " + std::to_string(shared->size()));
                (*shared)[i].boxes = build_merged_object_box_proxy(*(*shared)[i].voxels);
                (*shared)[i].voxels.reset();
            }
            job.report(1.0F, "Done");
        },
        [this, shared, disabled](LongJob&) {
            std::size_t objects = 0, boxes = 0;
            for (Target& target : *shared) {
                const EditorObject* object = workspace_.document().find_object(target.id);
                if (!object || !object->voxels || object->voxels->revision() != target.revision ||
                    static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(object->voxels.get())) != target.stamp) continue;
                CachedCollisionProxy& cached = collisionProxyCache_[target.id];
                cached.revision = target.revision;
                cached.objectStamp = target.stamp;
                cached.boxes = std::move(target.boxes);
                boxes += cached.boxes.size();
                ++objects;
            }
            set_status("Rebuilt collision for " + std::to_string(objects) + " object(s): " + std::to_string(boxes) +
                       " merged boxes, the same proxy Play uses" +
                       (disabled > 0 ? "; " + std::to_string(disabled) + " with Collision off skipped" : std::string()));
            return objects > 0;
        });
}

std::size_t NativeEditorController::validate_project() {
    if (longJob_) { set_status(long_job_busy_reason(), true); return 0; }
    const EditorDocument& live = workspace_.document();
    std::uint64_t workUnits = live.objects().size();
    for (const auto& [id, object] : live.objects())
        if (object.voxels && object.flags.collisionEnabled && !object.text3d && !object.gaborVolume) workUnits += object.voxels->occupied_voxel_count();
    // The checks read a snapshot of the scene (a copy when they run on the worker).
    const bool background = workUnits > longJobThreshold_;
    std::shared_ptr<const EditorDocument> document = background
        ? std::make_shared<const EditorDocument>(clone_editor_document(live))
        : std::shared_ptr<const EditorDocument>(&live, [](const EditorDocument*) {});
    std::size_t staleAssets = 0;
    for (const EditorAssetRecord& record : std::as_const(assetDatabase_).records())
        if (record.health != EditorAssetHealth::Current) ++staleAssets;
    struct ValidateResult { std::vector<EditorProblem> problems; std::size_t found{}; };
    auto result = std::make_shared<ValidateResult>();
    lastValidateFound_ = 0;
    return start_long_job(
        LongJobKind::ValidateProject, "Validate Project", workUnits,
        [document, root = projectRoot_, staleAssets, result](LongJob& job) {
            const auto add = [&](ProblemSeverity severity, std::string_view code, std::string summary, std::string details,
                                 std::optional<EditorObjectId> object) {
                result->problems.push_back({severity, std::string(code), std::move(summary), std::move(details), object, {}});
                ++result->found;
            };
            const auto resolve = [&](const std::filesystem::path& path) {
                return path.has_root_directory() || path.has_root_name() ? path : root / path;
            };
            std::error_code ec;
            std::string sceneError;
            job.report(0.02F, "Checking the scene");
            if (!document->validate(&sceneError))
                add(ProblemSeverity::Error, "validate.scene", "Scene is invalid: " + sceneError, "Saving will fail until this is fixed.", std::nullopt);
            static const ComponentTypeRegistry registry = ComponentTypeRegistry::make_default();
            const std::size_t total = std::max<std::size_t>(1, document->objects().size());
            std::size_t done = 0;
            for (const auto& [id, object] : document->objects()) {
                if ((++done & 0x3FU) == 0U) {
                    if (job.cancelled()) return;
                    job.report(0.05F + 0.9F * static_cast<float>(done) / static_cast<float>(total),
                               "Object " + std::to_string(done) + " of " + std::to_string(total));
                }
                const std::string name = object.name.empty() ? "object " + std::to_string(id) : object.name;
                if (!object.sourceAsset.empty() && !std::filesystem::exists(resolve(object.sourceAsset), ec))
                    add(ProblemSeverity::Warning, "validate.missing_file", name + ": source asset missing",
                        object.sourceAsset.generic_string() + " was not found; it cannot be reimported.", id);
                if (!object.textFontAsset.empty() && !std::filesystem::exists(resolve(object.textFontAsset), ec))
                    add(ProblemSeverity::Warning, "validate.missing_file", name + ": font missing",
                        object.textFontAsset.generic_string() + " was not found; the text cannot be recooked.", id);
                if (object.prefabLink && object.prefabLink->instanceRoot && !object.prefabLink->prefabAsset.empty() &&
                    !std::filesystem::exists(resolve(object.prefabLink->prefabAsset), ec))
                    add(ProblemSeverity::Error, "validate.missing_file", name + ": prefab file missing",
                        object.prefabLink->prefabAsset.generic_string() + " was not found; the instance cannot update.", id);
                for (const Component& component : object.components) {
                    std::string reason;
                    if (!registry.validate(component, &reason)) {
                        add(ProblemSeverity::Error, "validate.component", name + ": component '" + component.type + "' is invalid", reason, id);
                        continue;
                    }
                    const ComponentTypeSchema* schema = registry.find(component.type);
                    if (!schema) continue;
                    for (const ComponentPropertySchema& property : schema->properties) {
                        if (!property.assetReference) continue;
                        const auto it = component.properties.find(property.name);
                        const auto* path = it == component.properties.end() ? nullptr : std::get_if<std::string>(&it->second);
                        if (!path || path->empty()) {
                            if (property.required)
                                add(ProblemSeverity::Error, "validate.component", name + ": " + component.type + " has no " + property.name,
                                    "Set the " + property.name + " asset in the component.", id);
                            continue;
                        }
                        if (!std::filesystem::exists(resolve(*path), ec))
                            add(ProblemSeverity::Error, "validate.component", name + ": " + component.type + " references a missing file",
                                *path + " was not found.", id);
                    }
                }
                const bool voxelObject = object.voxels && !object.polygon && !object.text3d && !object.gaborVolume;
                if (voxelObject && object.voxels->occupied_voxel_count() > 0 && object.flags.collisionEnabled &&
                    !validate_box_proxy(*object.voxels, build_merged_object_box_proxy(*object.voxels)))
                    add(ProblemSeverity::Error, "validate.collision", name + ": collision proxy does not cover its voxels",
                        "Run Voxel > Rebuild Collision; report this if it persists.", id);
                if (voxelObject && object.voxels->occupied_voxel_count() == 0 && document->children_of(id).empty() &&
                    object.components.empty() && !object.prefabLink && !is_scatter_group(object))
                    add(ProblemSeverity::Information, "validate.empty_object", name + " has no voxels, children or components",
                        "An empty object does nothing in Play; delete it or give it content.", id);
            }
            if (staleAssets > 0)
                add(ProblemSeverity::Warning, "validate.assets", std::to_string(staleAssets) + " asset(s) are stale or broken",
                    "Open the Assets panel and choose Issues to see them.", std::nullopt);
            job.report(1.0F, "Done");
        },
        [this, result](LongJob&) {
            static constexpr std::array<std::string_view, 7> kCodes{
                "validate.scene", "validate.missing_file", "validate.component", "validate.empty_object",
                "validate.collision", "validate.assets", "validate.ok"};
            for (const std::string_view code : kCodes) workspace_.problems().clear_code(code);
            for (EditorProblem& problem : result->problems) workspace_.problems().add(std::move(problem));
            if (result->found == 0)
                workspace_.problems().add({ProblemSeverity::Information, "validate.ok", "Validation passed: no problems found",
                                           "Checked the scene, file references, components, collision and assets.", std::nullopt, {}});
            bottomTab_ = BottomPanelTab::Problems;
            recompute_layout();
            set_status(result->found == 0 ? "Validation passed: no problems found"
                                          : "Validation found " + std::to_string(result->found) + " problem(s); see Problems", result->found > 0);
            lastValidateFound_ = result->found;
            return true;
        }) ? lastValidateFound_ : 0;
}

// --- File > Open Scene / Open Project / Save As / Import Model --------------------------------

namespace {
std::string lowercase_copy(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string relative_label(const std::filesystem::path& path, const std::filesystem::path& root) {
    std::error_code ec;
    const auto relative = std::filesystem::relative(path, root, ec);
    if (ec || relative.empty() || *relative.begin() == "..") return path.generic_string();
    return relative.generic_string();
}

// Scene manifests under the project, skipping hidden, cooked and recovery folders.
std::vector<std::filesystem::path> find_scene_files(const std::filesystem::path& root) {
    std::vector<std::filesystem::path> scenes;
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(root, ec)) return scenes;
    auto it = std::filesystem::recursive_directory_iterator(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    for (const auto end = std::filesystem::recursive_directory_iterator(); !ec && it != end; it.increment(ec)) {
        const std::filesystem::path& path = it->path();
        const std::string name = path.filename().string();
        if (it->is_directory(ec)) {
            if (it.depth() >= 6 || name.starts_with('.') || name == "cooked" || name == "build" || name == "node_modules")
                it.disable_recursion_pending();
            continue;
        }
        if (path.extension() == ".dvescene" && it->is_regular_file(ec)) scenes.push_back(path);
        if (scenes.size() >= 2000) break;
    }
    std::sort(scenes.begin(), scenes.end());
    return scenes;
}

// Folders holding a project.dveproject: the current project, its siblings and children, and
// the working directory's children.
std::vector<std::filesystem::path> find_project_folders(const std::filesystem::path& current) {
    std::vector<std::filesystem::path> projects;
    std::error_code ec;
    const auto consider = [&](const std::filesystem::path& folder) {
        if (std::filesystem::is_regular_file(folder / "project.dveproject", ec) &&
            std::find(projects.begin(), projects.end(), folder) == projects.end())
            projects.push_back(folder);
    };
    const auto children = [&](const std::filesystem::path& folder) {
        if (folder.empty() || !std::filesystem::is_directory(folder, ec)) return;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
            if (entry.is_directory(ec)) consider(entry.path());
    };
    if (!current.empty()) {
        consider(current);
        children(current);
        children(current.parent_path());
    }
    children(std::filesystem::current_path(ec));
    std::sort(projects.begin(), projects.end());
    return projects;
}

// File > Save As target: <name> goes to <project>/scenes/<name>.dvescene, a relative path with
// folders is under the project, and a rooted path is taken as typed. The result must stay inside
// the project. "Rooted" uses has_root_directory()/has_root_name(), not is_absolute(): on Windows
// "/foo" is not is_absolute(), and appending it to the project root would leave the project.
std::optional<std::filesystem::path> scene_save_target(std::string_view nameOrPath, const std::filesystem::path& projectRoot,
                                                       std::string* error) {
    std::string name(nameOrPath);
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) name.erase(name.begin());
    const auto fail = [&](std::string message) -> std::optional<std::filesystem::path> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    if (name.empty()) return fail("Type a name for the scene");
    std::filesystem::path path(name);
    if (path.extension() != ".dvescene") path += ".dvescene";
    const std::string fileName = path.filename().string();
    if (path.stem().empty() || fileName.starts_with('.')) return fail("Type a name for the scene");
    for (const char c : fileName)
        if (static_cast<unsigned char>(c) < 32U || std::string_view("<>:\"|?*\\").find(c) != std::string_view::npos)
            return fail("A scene name cannot contain < > : \" | ? * or \\");
    const bool rooted = path.has_root_directory() || path.has_root_name();
    std::filesystem::path target = rooted ? path : path.has_parent_path() ? projectRoot / path : projectRoot / "scenes" / path;
    target = target.lexically_normal();
    if (!projectRoot.empty()) {
        const std::filesystem::path relative = target.lexically_relative(projectRoot.lexically_normal());
        if (relative.empty() || *relative.begin() == "..")
            return fail("Scenes are saved inside the project; " + target.generic_string() + " is outside " +
                        projectRoot.filename().string());
    }
    return target;
}

Quaternion quaternion_from_rotation_matrix(const Matrix4& m) {
    // Column-major: R(row, column) = values[column * 4 + row].
    const auto r = [&](int row, int column) { return m.values[static_cast<std::size_t>(column * 4 + row)]; };
    const float trace = r(0, 0) + r(1, 1) + r(2, 2);
    Quaternion q;
    if (trace > 0.0F) {
        const float s = std::sqrt(trace + 1.0F) * 2.0F;
        q = {(r(2, 1) - r(1, 2)) / s, (r(0, 2) - r(2, 0)) / s, (r(1, 0) - r(0, 1)) / s, 0.25F * s};
    } else if (r(0, 0) > r(1, 1) && r(0, 0) > r(2, 2)) {
        const float s = std::sqrt(1.0F + r(0, 0) - r(1, 1) - r(2, 2)) * 2.0F;
        q = {0.25F * s, (r(0, 1) + r(1, 0)) / s, (r(0, 2) + r(2, 0)) / s, (r(2, 1) - r(1, 2)) / s};
    } else if (r(1, 1) > r(2, 2)) {
        const float s = std::sqrt(1.0F + r(1, 1) - r(0, 0) - r(2, 2)) * 2.0F;
        q = {(r(0, 1) + r(1, 0)) / s, 0.25F * s, (r(1, 2) + r(2, 1)) / s, (r(0, 2) - r(2, 0)) / s};
    } else {
        const float s = std::sqrt(1.0F + r(2, 2) - r(0, 0) - r(1, 1)) * 2.0F;
        q = {(r(0, 2) + r(2, 0)) / s, (r(1, 2) + r(2, 1)) / s, 0.25F * s, (r(1, 0) - r(0, 1)) / s};
    }
    return normalize(q);
}
} // namespace

bool NativeEditorController::open_file_picker(FilePickerKind kind) {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) {
        set_status("Stop Play or Simulate first", true);
        return false;
    }
    cancel_text_edit();
    close_top_level_menu();
    close_context_menu();
    commandPaletteOpen_ = false;
    filePicker_ = {};
    filePicker_.open = true;
    filePicker_.kind = kind;
    const std::filesystem::path current = workspace_.document().path();
    if (kind == FilePickerKind::OpenProject) {
        for (const std::filesystem::path& folder : find_project_folders(projectRoot_)) {
            std::string name = folder.filename().string();
            if (auto project = EditorProject::load(folder / "project.dveproject")) name = project->name;
            filePicker_.entries.push_back({name, folder == projectRoot_ ? folder.generic_string() + "  (current)" : folder.generic_string(), folder});
        }
    } else {
        std::vector<std::filesystem::path> recent;
        if (auto project = EditorProject::load(projectRoot_ / "project.dveproject"))
            for (const auto& scene : project->recentScenes) recent.push_back(projectRoot_ / scene);
        const auto add = [&](const std::filesystem::path& path, std::string detail) {
            for (const FilePickerEntry& entry : filePicker_.entries) if (entry.path == path) return;
            if (!current.empty() && std::filesystem::path(path).lexically_normal() == current.lexically_normal())
                detail = detail.empty() ? "current" : detail + ", current";
            filePicker_.entries.push_back({relative_label(path, projectRoot_), std::move(detail), path});
        };
        std::error_code ec;
        for (const auto& path : recent) if (std::filesystem::exists(path, ec)) add(path, "recent");
        for (const auto& path : find_scene_files(projectRoot_)) add(path, {});
        if (kind == FilePickerKind::SaveSceneAs && !current.empty()) {
            // Prefill so Enter targets the current file (keeping its folder when not in scenes/).
            std::filesystem::path relative = std::filesystem::relative(current, projectRoot_, ec);
            if (ec || relative.empty() || *relative.begin() == "..") relative = current;
            relative.replace_extension();
            filePicker_.query = relative.parent_path() == "scenes" ? relative.filename().string() : relative.generic_string();
        }
    }
    if (kind == FilePickerKind::SaveSceneAs) {
        filePicker_.message = "Type a scene name; it is saved as scenes/<name>.dvescene in " + projectRoot_.filename().string();
    } else if (filePicker_.entries.empty()) {
        filePicker_.message = kind == FilePickerKind::OpenScene
            ? "No scenes in this project yet; type a path to a .dvescene file"
            : "No projects found next to this one; type a project folder path";
    }
    set_status(kind == FilePickerKind::OpenScene ? "Open Scene: type to filter, Enter opens, Esc closes"
               : kind == FilePickerKind::OpenProject ? "Open Project: type to filter or a folder path, Enter opens"
                                                     : "Save As: type a name, Enter saves, Esc closes");
    return true;
}

void NativeEditorController::close_file_picker() { filePicker_ = {}; }

std::vector<std::size_t> NativeEditorController::file_picker_matches() const {
    std::vector<std::size_t> matches;
    // Save As shows every scene (for reference / replacing); the others filter by the query.
    const std::string query = filePicker_.kind == FilePickerKind::SaveSceneAs ? std::string() : lowercase_copy(filePicker_.query);
    for (std::size_t i = 0; i < filePicker_.entries.size(); ++i) {
        const FilePickerEntry& entry = filePicker_.entries[i];
        if (query.empty() || lowercase_copy(entry.label).find(query) != std::string::npos ||
            lowercase_copy(entry.path.generic_string()).find(query) != std::string::npos)
            matches.push_back(i);
    }
    return matches;
}

NativeFilePickerLayout NativeEditorController::file_picker_layout() const {
    NativeFilePickerLayout result;
    result.panel.width = std::min(760, std::max(480, width_ - 60));
    result.panel.height = std::min(480, std::max(300, height_ - 80));
    result.panel.x = (width_ - result.panel.width) / 2;
    result.panel.y = std::max(32, (height_ - result.panel.height) / 3);
    result.searchBox = {result.panel.x + 16, result.panel.y + 40, result.panel.width - 32, 34};
    result.hintBar = {result.panel.x + 12, result.panel.y + result.panel.height - 36, result.panel.width - 24, 28};
    const int rowHeight = 24;
    const int bodyY = result.searchBox.y + result.searchBox.height + 28;
    const std::size_t visible = static_cast<std::size_t>(std::max(1, (result.hintBar.y - 6 - bodyY) / rowHeight));
    const std::size_t count = file_picker_matches().size();
    if (filePicker_.selected >= visible) result.firstVisibleRow = filePicker_.selected - visible + 1U;
    for (std::size_t index = result.firstVisibleRow; index < count && result.rows.size() < visible; ++index)
        result.rows.push_back({result.panel.x + 12, bodyY + static_cast<int>(result.rows.size()) * rowHeight,
                               result.panel.width - 24, rowHeight - 2});
    return result;
}

bool NativeEditorController::handle_file_picker_key(std::string_view normalized) {
    const std::size_t count = file_picker_matches().size();
    if (normalized == "escape") { close_file_picker(); set_status("Closed"); return true; }
    if (normalized == "up") { if (count) filePicker_.selected = filePicker_.selected == 0 ? count - 1 : filePicker_.selected - 1; return true; }
    if (normalized == "down") { if (count) filePicker_.selected = (filePicker_.selected + 1) % count; return true; }
    if (normalized == "backspace") {
        if (!filePicker_.query.empty()) filePicker_.query.pop_back();
        filePicker_.selected = 0;
        filePicker_.confirmReplace.reset();
        return true;
    }
    if (normalized == "return" || normalized == "enter" || normalized == "kp_enter") { (void)accept_file_picker(); return true; }
    return true;  // the picker is modal: other keys do nothing else
}

bool NativeEditorController::accept_file_picker() {
    if (!filePicker_.open) return false;
    const std::vector<std::size_t> matches = file_picker_matches();
    const auto fail = [&](std::string message) {
        filePicker_.message = std::move(message);
        filePicker_.messageIsError = true;
        set_status(filePicker_.message, true);
        return false;
    };
    std::error_code ec;
    if (filePicker_.kind == FilePickerKind::SaveSceneAs) {
        std::string name = filePicker_.query;
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
        if (name.empty() && !matches.empty()) name = filePicker_.entries[matches[std::min(filePicker_.selected, matches.size() - 1)]].path.string();
        std::string invalid;
        const auto resolved = scene_save_target(name, projectRoot_, &invalid);
        if (!resolved) return fail(invalid);
        const std::filesystem::path target = *resolved;
        // The confirmation is for this exact file: a different selection or name asks again.
        const bool replace = filePicker_.confirmReplace && filePicker_.confirmReplace->lexically_normal() == target;
        if (!replace && std::filesystem::exists(target, ec) &&
            target.lexically_normal() != workspace_.document().path().lexically_normal()) {
            filePicker_.confirmReplace = target;
            filePicker_.message = relative_label(target, projectRoot_) + " already exists: Enter again to replace it, or edit the name";
            filePicker_.messageIsError = true;
            return false;
        }
        close_file_picker();
        return save_scene_as(target.string(), true);
    }
    std::optional<std::filesystem::path> chosen;
    if (!matches.empty()) chosen = filePicker_.entries[matches[std::min(filePicker_.selected, matches.size() - 1)]].path;
    else if (!filePicker_.query.empty()) {
        std::filesystem::path typed(filePicker_.query);
        // has_root_directory(), not is_relative(): on Windows "/foo" is relative (no drive).
        if (!typed.has_root_directory() && !typed.has_root_name()) typed = projectRoot_ / typed;
        const bool usable = filePicker_.kind == FilePickerKind::OpenScene
            ? typed.extension() == ".dvescene" && std::filesystem::is_regular_file(typed, ec)
            : std::filesystem::is_regular_file(typed / "project.dveproject", ec);
        if (usable) chosen = typed;
    }
    if (!chosen) return fail(filePicker_.kind == FilePickerKind::OpenScene ? "No scene matches; type part of a name or a path"
                                                                           : "No project matches; type a folder that holds project.dveproject");
    const FilePickerKind kind = filePicker_.kind;
    close_file_picker();
    return kind == FilePickerKind::OpenScene ? open_scene_file(*chosen) : open_project_folder(*chosen);
}

void NativeEditorController::remember_recent_scene(const std::filesystem::path& path) {
    const std::filesystem::path file = projectRoot_ / "project.dveproject";
    auto project = EditorProject::load(file);
    if (!project) return;
    std::error_code ec;
    const auto relative = std::filesystem::relative(path, projectRoot_, ec);
    if (ec || relative.empty() || *relative.begin() == "..") return;
    std::erase(project->recentScenes, relative);
    project->recentScenes.insert(project->recentScenes.begin(), relative);
    if (project->recentScenes.size() > 10) project->recentScenes.resize(10);
    std::string error;
    if (!project->save(&error)) workspace_.log().add(EditorLogLevel::Warning, "Recent scenes: " + error);
}

bool NativeEditorController::open_scene_file(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) { set_status("Scene not found: " + path.generic_string(), true); return false; }
    if (workspace_.document().dirty() && workspace_.preferences().confirmDestructiveActions) {
        pendingOpenPath_ = path;
        pendingDestructiveAction_ = PendingDestructiveAction::OpenScene;
        set_status(std::string(pending_confirmation_message()), false, 0.0F);
        return true;
    }
    return open_scene_now(path);
}

bool NativeEditorController::open_scene_now(const std::filesystem::path& path) {
    std::string error;
    auto loaded = EditorDocument::load(path, &error);
    if (!loaded) { set_status("Could not open " + path.filename().string() + ": " + error, true, 8.0F); return false; }
    cancel_scatter();
    cancel_voxel_join();
    if (voxelBoolean_.active()) cancel_voxel_boolean();
    cancel_voxel_slice();
    cancel_text_edit();
    if (gizmoDragging_) finish_gizmo_drag(true);
    floodFillArmed_ = false;
    followSelection_ = false;
    followAnchor_.reset();
    reset_hierarchy_view();
    workspace_.document() = std::move(*loaded);
    workspace_.commands().clear();
    workspace_.clear_selection();
    reset_voxel_selection_state();  // ids restart per scene: never carry cells to another object
    openGroups_.clear(); pivotEditing_ = false;  // ids restart per scene
    viewportSettings_.isolatedObjects.clear();
    collisionProxyCache_.clear();
    (void)dispatch_action("view.frame_all");
    remember_recent_scene(path);
    recompute_layout();
    refresh_menu_state();
    set_status("Opened " + relative_label(path, projectRoot_));
    return true;
}

bool NativeEditorController::save_scene_as(std::string_view nameOrPath, bool replaceExisting) {
    std::string invalid;
    const auto resolved = scene_save_target(nameOrPath, projectRoot_, &invalid);
    if (!resolved) { set_status("Save As: " + invalid, true); return false; }
    const std::filesystem::path target = *resolved;
    std::error_code ec;
    if (!replaceExisting && std::filesystem::exists(target, ec)) {
        set_status(relative_label(target, projectRoot_) + " already exists", true);
        return false;
    }
    std::filesystem::create_directories(target.parent_path(), ec);
    const std::filesystem::path previousRecovery = autosave_directory();  // keyed by the old name
    const auto result = workspace_.document().save_transactional(target);
    if (!result.success) { set_status("Save As failed: " + result.error, true, 8.0F); return false; }
    // The saved file holds everything the old recovery copy did (and the new name has none yet).
    std::filesystem::remove_all(previousRecovery, ec);
    std::filesystem::remove_all(autosave_directory(), ec);
    autosaveElapsedSeconds_ = 0.0;
    ++manualSaveCount_;
    remember_recent_scene(target);
    refresh_menu_state();
    set_status("Saved as " + relative_label(target, projectRoot_));
    return true;
}

bool NativeEditorController::open_project_folder(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(root / "project.dveproject", ec)) {
        set_status(root.generic_string() + " is not a project (no project.dveproject)", true);
        return false;
    }
    if (workspace_.document().dirty() && workspace_.preferences().confirmDestructiveActions) {
        pendingOpenPath_ = root;
        pendingDestructiveAction_ = PendingDestructiveAction::OpenProject;
        set_status(std::string(pending_confirmation_message()), false, 0.0F);
        return true;
    }
    return open_project_now(root);
}

bool NativeEditorController::open_project_now(const std::filesystem::path& root) {
    configure_ai_assistant(root);  // activates the project: asset index, workspace state, tools
    std::filesystem::path scene;
    std::error_code ec;
    if (auto project = EditorProject::load(root / "project.dveproject"))
        for (const auto& recent : project->recentScenes)
            if (std::filesystem::is_regular_file(root / recent, ec)) { scene = root / recent; break; }
    if (scene.empty()) {
        const auto scenes = find_scene_files(root);
        if (!scenes.empty()) scene = scenes.front();
    }
    if (!scene.empty() && open_scene_now(scene)) {
        set_status("Opened project " + root.filename().string() + " (" + relative_label(scene, root) + ")");
        return true;
    }
    create_new_scene_now();
    set_status("Opened project " + root.filename().string() + " with a new scene (it has no scenes yet)");
    return true;
}

// --- File > Import Model / Reimport Model (options panel, long job) ---------------------------

namespace {
constexpr std::array<float, 13> kImportVoxelSizes{0.005F, 0.01F, 0.02F, 0.025F, 0.05F, 0.1F, 0.2F, 0.25F, 0.5F, 1.0F, 2.0F, 5.0F, 10.0F};
constexpr std::array<float, 11> kImportScales{0.001F, 0.01F, 0.1F, 0.25F, 0.5F, 1.0F, 2.0F, 4.0F, 10.0F, 100.0F, 1000.0F};
constexpr std::size_t kImportSettingRows = 3;  // voxel size, scale, fill; mesh rows follow

std::string format_meters(float value) {
    char text[32];
    std::snprintf(text, sizeof text, "%.4g m", static_cast<double>(value));
    return text;
}
std::string format_scale(float value) {
    char text[32];
    std::snprintf(text, sizeof text, "%.4gx", static_cast<double>(value));
    return text;
}
std::string format_thousands(std::uint64_t value) {
    std::string digits = std::to_string(value);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) digits.insert(static_cast<std::size_t>(i), ",");
    return digits;
}
std::optional<float> parse_float(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.back())) || text.back() == 'm' || text.back() == 'x'))
        text.remove_suffix(1);
    if (text.empty()) return std::nullopt;
    // strtod on a copy: std::from_chars for float is not in every standard library this builds with.
    const std::string copy(text);
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || !std::isfinite(value)) return std::nullopt;
    return static_cast<float>(value);
}

// An import group: the object itself (or its nearest ancestor) that came from a model file and
// is the top of that import (a group holding the settings, or whose parent is from elsewhere).
std::optional<EditorObjectId> find_import_root(const EditorDocument& document, EditorObjectId id) {
    const EditorObject* object = document.find_object(id);
    std::optional<EditorObjectId> found;
    for (int depth = 0; object && depth < 64; ++depth) {
        if (read_import_settings(*object)) return object->id;
        if (!object->sourceAsset.empty() && is_model_source(object->sourceAsset)) {
            const EditorObject* parent = object->parent ? document.find_object(*object->parent) : nullptr;
            if (!parent || parent->sourceAsset != object->sourceAsset) return object->id;
            found = object->id;
        }
        object = object->parent ? document.find_object(*object->parent) : nullptr;
    }
    return found;
}

std::unique_ptr<VoxelObject> voxel_copy_with_id(const VoxelObject& source, EditorObjectId id) {
    // EditorDocument::add_object rebuilds a voxel object whose id differs; doing it here keeps
    // that per-voxel copy on the import worker instead of the UI thread.
    auto copy = std::make_unique<VoxelObject>(id);
    for (const auto& entry : source.bricks()) {
        const BrickKey key = entry.first;
        const Brick& brick = entry.second;
        brick.occupancy().for_each_set([&](std::uint16_t index) {
            copy->set_voxel(global_from_local(key, local_from_index_unchecked(index)), brick.material(index));
        });
    }
    return copy;
}
} // namespace

bool NativeEditorController::import_selected_model() {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) { set_status("Stop Play or Simulate before importing", true); return false; }
    const EditorAssetRecord* asset = assetBrowserState_.selectedId ? std::as_const(assetDatabase_).find(*assetBrowserState_.selectedId) : nullptr;
    if (!asset || !is_model_source(asset->relativePath)) {
        set_status("Select an OBJ, glTF or GLB model in the Assets panel, then Import Model", true, 6.0F);
        return false;
    }
    return open_import_options(asset->relativePath);
}

bool NativeEditorController::open_import_options(const std::filesystem::path& relativeSource) {
    return open_import_options_for(relativeSource, false, 0, EditorImportSettings{});
}

bool NativeEditorController::open_import_options_for(const std::filesystem::path& relativeSource, bool reimport,
                                                     EditorObjectId target, EditorImportSettings settings) {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) { set_status("Stop Play or Simulate before importing", true); return false; }
    if (longJob_) { set_status(long_job_busy_reason(), true); return false; }
    if (!is_model_source(relativeSource)) { set_status("Import Model reads OBJ, glTF and GLB files", true); return false; }
    const std::filesystem::path source = relativeSource.has_root_directory() || relativeSource.has_root_name()
        ? relativeSource : projectRoot_ / relativeSource;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(source, ec)) {
        set_status("Import: " + relativeSource.generic_string() + " was not found", true, 6.0F);
        return false;
    }
    // A modal panel: close menus, text edits and any preview first (one preview at a time).
    close_import_options();
    close_file_picker();
    cancel_text_edit();
    close_top_level_menu();
    close_context_menu();
    commandPaletteOpen_ = false;
    floodFillArmed_ = false;
    close_previews_for_voxel_edit(reimport ? "Reimport started" : "Import started");
    const std::uint64_t bytes = std::filesystem::file_size(source, ec);
    auto model = std::make_shared<ImportedModelResult>();
    const std::string fileName = source.filename().string();
    // Take a pending Assets drop now so a cancelled read cannot leak it into a later Import Model.
    const std::optional<Float3> dropPoint = reimport ? std::nullopt : std::exchange(pendingImportDrop_, std::nullopt);
    return start_long_job(
        LongJobKind::ReadModel, "Read " + fileName, ec ? 0 : bytes / 4,
        [model, source, fileName](LongJob& job) {
            job.report(0.05F, "Reading " + fileName);
            *model = import_model(source);
            job.report(1.0F, "Read " + fileName);
        },
        [this, model, relativeSource, reimport, target, settings, dropPoint](LongJob&) mutable {
            if (!model->success) {
                std::string reason = "the file could not be read";
                for (const ImportDiagnostic& d : model->diagnostics)
                    if (d.severity == ImportDiagnostic::Severity::Error) { reason = d.message; break; }
                set_status("Import failed: " + reason, true, 8.0F);
                return false;
            }
            importOptions_ = {};
            importOptions_.open = true;
            importOptions_.reimport = reimport;
            importOptions_.reimportTarget = target;
            importOptions_.relativeSource = relativeSource;
            importOptions_.nodes = list_import_nodes(model->scene);
            importOptions_.model = std::move(model);
            // Keep only left-out names the file still has.
            std::erase_if(settings.excludedNodes, [&](const std::string& name) {
                return std::none_of(importOptions_.nodes.begin(), importOptions_.nodes.end(),
                                    [&](const EditorImportNode& node) { return node.name == name; });
            });
            importOptions_.settings = std::move(settings);
            importOptions_.dropPoint = dropPoint;
            refresh_import_estimate();
            set_status(std::string(reimport ? "Reimport" : "Import") +
                       " options: Up/Down pick a row, Left/Right or type a value, Space toggles a mesh, Enter imports, Esc cancels",
                       false, 10.0F);
            return true;
        });
}

void NativeEditorController::close_import_options(std::string reason) {
    if (!importOptions_.open) return;
    importOptions_ = {};
    if (!reason.empty()) set_status(std::move(reason));
}

void NativeEditorController::refresh_import_estimate() {
    if (!importOptions_.model) return;
    importOptions_.estimate = estimate_import(importOptions_.model->scene, importOptions_.settings);
    if (!importOptions_.estimate.refusal.empty()) {
        importOptions_.message = importOptions_.estimate.refusal;
        importOptions_.messageIsError = true;
    } else if (importOptions_.messageIsError) {
        importOptions_.message.clear();
        importOptions_.messageIsError = false;
    }
}

std::vector<std::pair<std::string, std::string>> NativeEditorController::import_option_rows() const {
    std::vector<std::pair<std::string, std::string>> rows;
    if (!importOptions_.open) return rows;
    const auto typed = [&](std::size_t row, std::string value) {
        return importOptions_.selected == row && !importOptions_.typed.empty() ? importOptions_.typed + "_" : std::move(value);
    };
    rows.emplace_back("Voxel size", typed(0, format_meters(importOptions_.settings.voxelSizeMeters)));
    rows.emplace_back("Scale", typed(1, format_scale(importOptions_.settings.scale)));
    rows.emplace_back("Fill", std::string(import_mode_label(importOptions_.settings.mode)));
    for (const EditorImportNode& node : importOptions_.nodes)
        rows.emplace_back((importOptions_.settings.excludedNodes.contains(node.name) ? "[ ] " : "[x] ") + node.path,
                          format_thousands(node.triangles) + " triangles");
    return rows;
}

bool NativeEditorController::set_import_option(ImportOptionsRow row, std::string_view text, std::size_t node) {
    if (!importOptions_.open) return false;
    const auto fail = [&](std::string message) {
        importOptions_.message = std::move(message);
        importOptions_.messageIsError = true;
        set_status(importOptions_.message, true);
        return false;
    };
    EditorImportSettings& settings = importOptions_.settings;
    switch (row) {
        case ImportOptionsRow::VoxelSize: {
            const auto value = parse_float(text);
            if (!value || *value < kMinImportVoxelSize || *value > kMaxImportVoxelSize)
                return fail("Voxel size must be a number of meters from 0.005 to 10");
            settings.voxelSizeMeters = *value;
            break;
        }
        case ImportOptionsRow::Scale: {
            const auto value = parse_float(text);
            if (!value || *value < kMinImportScale || *value > kMaxImportScale) return fail("Scale must be a number from 0.001 to 1000");
            settings.scale = *value;
            break;
        }
        case ImportOptionsRow::Mode: {
            const std::string mode = lowercase_copy(text);
            if (mode == "solid") settings.mode = VoxelizationMode::Solid;
            else if (mode == "shell") settings.mode = VoxelizationMode::Shell;
            else if (mode == "surface" || mode == "surface only") settings.mode = VoxelizationMode::SurfaceOnly;
            else return fail("Fill is Solid, Shell or Surface");
            break;
        }
        case ImportOptionsRow::Node: {
            if (node >= importOptions_.nodes.size()) return fail("No such mesh");
            const std::string& name = importOptions_.nodes[node].name;
            const std::string value = lowercase_copy(text);
            const bool include = value == "toggle" ? settings.excludedNodes.contains(name) : value == "on" || value == "include";
            if (include) settings.excludedNodes.erase(name);
            else settings.excludedNodes.insert(name);
            break;
        }
    }
    importOptions_.typed.clear();
    if (!importOptions_.messageIsError || importOptions_.message != importOptions_.estimate.refusal) importOptions_.message.clear();
    importOptions_.messageIsError = false;
    refresh_import_estimate();
    return true;
}

void NativeEditorController::nudge_import_option(std::size_t row, int steps) {
    if (!importOptions_.open || steps == 0) return;
    EditorImportSettings& settings = importOptions_.settings;
    const auto step = [&](const auto& list, float current) {
        // The next preset above (or below) the current value.
        if (steps > 0) {
            for (const float value : list) if (value > current * 1.0001F) return value;
            return list.back();
        }
        for (auto it = list.rbegin(); it != list.rend(); ++it) if (*it < current * 0.9999F) return *it;
        return list.front();
    };
    importOptions_.typed.clear();
    if (row == 0) settings.voxelSizeMeters = step(kImportVoxelSizes, settings.voxelSizeMeters);
    else if (row == 1) settings.scale = step(kImportScales, settings.scale);
    else if (row == 2) {
        const int mode = (static_cast<int>(settings.mode) + (steps > 0 ? 1 : 2)) % 3;
        settings.mode = static_cast<VoxelizationMode>(mode);
    } else if (row - kImportSettingRows < importOptions_.nodes.size()) {
        (void)set_import_option(ImportOptionsRow::Node, "toggle", row - kImportSettingRows);
        return;
    }
    refresh_import_estimate();
}

NativeImportOptionsLayout NativeEditorController::import_options_layout() const {
    NativeImportOptionsLayout result;
    result.panel.width = std::min(720, std::max(480, width_ - 60));
    result.panel.height = std::min(500, std::max(340, height_ - 80));
    result.panel.x = (width_ - result.panel.width) / 2;
    result.panel.y = std::max(32, (height_ - result.panel.height) / 3);
    result.hintBar = {result.panel.x + 12, result.panel.y + result.panel.height - 36, result.panel.width - 24, 28};
    const int buttonY = result.hintBar.y - 40;
    result.cancelButton = {result.panel.x + result.panel.width - 16 - 96, buttonY, 96, 30};
    result.importButton = {result.cancelButton.x - 12 - 120, buttonY, 120, 30};
    result.estimateBox = {result.panel.x + 16, buttonY - 58, result.panel.width - 32, 50};
    const int rowHeight = 24;
    const int bodyY = result.panel.y + 44;
    const int visible = std::max(1, (result.estimateBox.y - 8 - bodyY) / rowHeight);
    const std::size_t total = kImportSettingRows + importOptions_.nodes.size();
    std::size_t first = 0;
    if (importOptions_.selected >= static_cast<std::size_t>(visible)) first = importOptions_.selected - static_cast<std::size_t>(visible) + 1U;
    for (std::size_t index = first; index < total && result.rows.size() < static_cast<std::size_t>(visible); ++index) {
        result.rows.push_back({result.panel.x + 12, bodyY + static_cast<int>(result.rows.size()) * rowHeight, result.panel.width - 24, rowHeight - 2});
        result.rowIndex.push_back(index);
    }
    return result;
}

bool NativeEditorController::handle_import_options_key(std::string_view normalized) {
    const std::size_t total = kImportSettingRows + importOptions_.nodes.size();
    std::size_t& selected = importOptions_.selected;
    if (normalized == "escape") {
        if (!importOptions_.typed.empty()) { importOptions_.typed.clear(); return true; }
        close_import_options(importOptions_.reimport ? "Reimport cancelled; the scene is unchanged" : "Import cancelled; the scene is unchanged");
        return true;
    }
    if (normalized == "up") { importOptions_.typed.clear(); selected = selected == 0 ? total - 1 : selected - 1; return true; }
    if (normalized == "down" || normalized == "tab") { importOptions_.typed.clear(); selected = (selected + 1) % total; return true; }
    if (normalized == "left") { nudge_import_option(selected, -1); return true; }
    if (normalized == "right") { nudge_import_option(selected, 1); return true; }
    if (normalized == "space") { if (selected >= 2) nudge_import_option(selected, 1); return true; }
    if (normalized == "backspace") { if (!importOptions_.typed.empty()) importOptions_.typed.pop_back(); return true; }
    if (normalized == "return" || normalized == "enter" || normalized == "kp_enter") {
        if (!importOptions_.typed.empty()) {
            const std::string typed = importOptions_.typed;
            (void)set_import_option(selected == 0 ? ImportOptionsRow::VoxelSize : ImportOptionsRow::Scale, typed);
            return true;
        }
        (void)accept_import_options();
        return true;
    }
    return true;  // modal: other keys do nothing else
}

bool NativeEditorController::accept_import_options() {
    if (!importOptions_.open) return false;
    if (!importOptions_.typed.empty()) {
        const std::string typed = importOptions_.typed;
        if (!set_import_option(importOptions_.selected == 0 ? ImportOptionsRow::VoxelSize : ImportOptionsRow::Scale, typed)) return false;
    }
    refresh_import_estimate();
    if (!importOptions_.estimate.refusal.empty()) {
        importOptions_.message = importOptions_.estimate.refusal;
        importOptions_.messageIsError = true;
        set_status(importOptions_.estimate.refusal, true, 8.0F);
        return false;
    }
    return start_import_job(importOptions_.reimport);
}

std::string NativeEditorController::reimport_disabled_reason() const {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) return "Stop Play or Simulate before reimporting.";
    const auto selected = workspace_.selected_object();
    if (!selected) return "Select an imported model (its group or one of its parts) first.";
    const EditorDocument& document = workspace_.document();
    const auto root = find_import_root(document, *selected);
    if (!root) return "The selection was not imported from a model file.";
    const EditorObject* group = document.find_object(*root);
    for (const EditorObjectId id : collect_editor_object_subtree_ids(document, std::vector<EditorObjectId>{*root})) {
        const EditorObject* object = document.find_object(id);
        if (!object) continue;
        if (object->flags.locked) return "Unlock " + object->name + " before reimporting " + group->name + ".";
        if (!object->flags.visible || is_isolated_out(id))
            return object->name + " is hidden or isolated out of view; show it before reimporting " + group->name + ".";
    }
    std::error_code ec;
    const std::filesystem::path source = group->sourceAsset.has_root_directory() || group->sourceAsset.has_root_name()
        ? group->sourceAsset : projectRoot_ / group->sourceAsset;
    if (!std::filesystem::is_regular_file(source, ec)) return group->sourceAsset.generic_string() + " was not found.";
    return {};
}

bool NativeEditorController::reimport_selected_model() {
    if (const std::string reason = reimport_disabled_reason(); !reason.empty()) { set_status(reason, true, 6.0F); return false; }
    const EditorDocument& document = workspace_.document();
    const EditorObjectId root = *find_import_root(document, *workspace_.selected_object());
    const EditorObject* group = document.find_object(root);
    EditorImportSettings settings = read_import_settings(*group).value_or(EditorImportSettings{});
    if (!read_import_settings(*group)) {
        // Imported before the options existed: start from the voxel size the parts have.
        for (const auto& [id, object] : document.objects())
            if (object.parent == root && object.voxels) { settings.voxelSizeMeters = object.voxelSizeMeters; break; }
    }
    return open_import_options_for(group->sourceAsset, true, root, std::move(settings));
}

namespace {
std::string normalized_material_name(std::string_view name) {
    std::string result;
    for (const char c : name)
        if (std::isalnum(static_cast<unsigned char>(c))) result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return result;
}
bool importable_model(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return extension == ".obj" || extension == ".gltf" || extension == ".glb";
}
EditorObjectBounds union_bounds(EditorObjectBounds a, const EditorObjectBounds& b) {
    if (!b.valid) return a;
    if (!a.valid) return b;
    a.minimum = {std::min(a.minimum.x, b.minimum.x), std::min(a.minimum.y, b.minimum.y), std::min(a.minimum.z, b.minimum.z)};
    a.maximum = {std::max(a.maximum.x, b.maximum.x), std::max(a.maximum.y, b.maximum.y), std::max(a.maximum.z, b.maximum.z)};
    return a;
}
// The offset that puts the bottom centre of `bounds` on `point`.
Float3 bottom_centre_offset(const EditorObjectBounds& bounds, Float3 point) {
    if (!bounds.valid) return point;
    return {point.x - (bounds.minimum.x + bounds.maximum.x) * 0.5F, point.y - bounds.minimum.y,
            point.z - (bounds.minimum.z + bounds.maximum.z) * 0.5F};
}
EditorObjectBounds offset_bounds(EditorObjectBounds bounds, Float3 offset) {
    bounds.minimum = add(bounds.minimum, offset);
    bounds.maximum = add(bounds.maximum, offset);
    return bounds;
}
}  // namespace

bool NativeEditorController::start_import_job(bool reimport) {
    const ImportOptionsState options = importOptions_;
    EditorDocument& document = workspace_.document();
    if (reimport) {
        if (!document.find_object(options.reimportTarget)) { close_import_options("Reimport: the model group was deleted"); return false; }
        if (const std::string reason = reimport_disabled_reason(); !reason.empty()) {
            importOptions_.message = reason;
            importOptions_.messageIsError = true;
            set_status(reason, true, 6.0F);
            return false;
        }
    }
    // Ids are reserved now (cheap, and they do not change the scene) so the worker can build the
    // parts with their final ids; a cancelled import just leaves the ids unused.
    const EditorObjectId groupId = reimport ? options.reimportTarget : document.allocate_object_id();
    std::vector<EditorObjectId> partIds(options.estimate.includedNodes);
    for (EditorObjectId& id : partIds) id = document.allocate_object_id();
    struct ImportResult {
        std::vector<EditorObject> parts;
        std::vector<std::string> warnings;
        std::string error;
        std::uint64_t voxels{};
        float voxelSize{};
    };
    auto result = std::make_shared<ImportResult>();
    const std::filesystem::path relativeSource = options.relativeSource;
    const std::string fileName = relativeSource.filename().string();
    const std::string label = (reimport ? "Reimport " : "Import ") + fileName;
    importOptions_ = {};
    return start_long_job(
        reimport ? LongJobKind::ReimportModel : LongJobKind::ImportModel, label, options.estimate.voxels,
        [model = options.model, settings = options.settings, partIds, groupId, relativeSource, result](LongJob& job) {
            job.report(0.02F, "Preparing");
            ImportedScene scene = scaled_import_scene(model->scene, settings.scale);
            const SceneCookSettings cook = import_cook_settings(scene, settings);
            std::vector<std::optional<std::uint32_t>> meshes(scene.nodes.size());
            std::vector<std::uint32_t> included;
            for (std::uint32_t i = 0; i < scene.nodes.size(); ++i) {
                meshes[i] = scene.nodes[i].mesh;
                if (meshes[i] && *meshes[i] < scene.meshes.size() && !settings.excludedNodes.contains(scene.nodes[i].name))
                    included.push_back(i);
            }
            std::map<std::uint32_t, EditorObjectId> nodeToPart;
            std::vector<std::uint32_t> partNodes;
            // One node at a time, so progress is per mesh and Cancel stops between meshes.
            for (std::size_t k = 0; k < included.size() && k < partIds.size(); ++k) {
                if (job.cancelled()) return;
                const std::uint32_t nodeIndex = included[k];
                const std::string name = scene.nodes[nodeIndex].name.empty() ? "Part " + std::to_string(k + 1) : scene.nodes[nodeIndex].name;
                job.report(0.05F + 0.9F * static_cast<float>(k) / static_cast<float>(included.size()),
                           "Voxelizing " + std::to_string(k + 1) + " of " + std::to_string(included.size()) + ": " + name);
                for (std::uint32_t j = 0; j < scene.nodes.size(); ++j) scene.nodes[j].mesh = j == nodeIndex ? meshes[j] : std::nullopt;
                CookedVoxelScene cooked = voxelize_scene_objects(scene, cook);
                for (const ImportDiagnostic& d : cooked.diagnostics) {
                    if (d.code == "NO_COOKED_OBJECTS") continue;
                    if (d.severity == ImportDiagnostic::Severity::Error) { result->error = d.message; return; }
                    if (d.severity == ImportDiagnostic::Severity::Warning) result->warnings.push_back(d.message);
                }
                for (CookedVoxelObject& part : cooked.objects) {
                    if (part.sourceNode != nodeIndex) continue;
                    const EditorObjectId id = partIds[k];
                    EditorObject object(id, part.name.empty() ? name : part.name);
                    object.voxelSizeMeters = part.asset.voxelSizeMeters;
                    object.voxelScale = part.asset.voxelScale;
                    object.voxels = voxel_copy_with_id(part.asset.object, id);
                    object.transform = make_rigid_transform({part.worldTransform.values[12], part.worldTransform.values[13], part.worldTransform.values[14]},
                                                            quaternion_from_rotation_matrix(part.worldTransform));
                    object.flags.anchored = part.anchored;
                    object.flags.structural = part.structural;
                    object.flags.collisionEnabled = part.generateCollision;
                    object.sourceAsset = relativeSource;
                    result->voxels += object.voxels->occupied_voxel_count();
                    result->voxelSize = part.asset.voxelSizeMeters;
                    nodeToPart[nodeIndex] = id;
                    partNodes.push_back(nodeIndex);
                    result->parts.push_back(std::move(object));
                    break;
                }
            }
            if (job.cancelled()) return;
            job.report(0.97F, "Linking parts");
            // Parent each part to the nearest imported ancestor mesh, else to the group.
            for (std::size_t i = 0; i < result->parts.size(); ++i) {
                result->parts[i].parent = groupId;
                for (auto parent = model->scene.nodes[partNodes[i]].parent; parent && *parent < model->scene.nodes.size();
                     parent = model->scene.nodes[*parent].parent) {
                    if (const auto it = nodeToPart.find(*parent); it != nodeToPart.end()) { result->parts[i].parent = it->second; break; }
                }
            }
            if (result->voxels > kMaxImportVoxels) result->error = "the model made more than " + format_thousands(kMaxImportVoxels) + " voxels";
            job.report(1.0F, "Done");
        },
        [this, result, groupId, reimport, settings = options.settings, relativeSource, label, fileName,
         dropPoint = options.dropPoint](LongJob&) {
            if (!result->error.empty()) { set_status(label + " failed: " + result->error, true, 8.0F); return false; }
            if (result->parts.empty() || result->voxels == 0) { set_status(label + " failed: the model produced no voxels", true, 8.0F); return false; }
            EditorDocument& document = workspace_.document();
            auto command = std::make_unique<CompoundCommand>(label);
            RigidTransform placement{};
            if (reimport) {
                const EditorObject* group = document.find_object(groupId);
                if (!group) { set_status("Reimport: the model group was deleted", true, 6.0F); return false; }
                placement = group->transform;
                EditorObject replacement = clone_editor_object(*group);
                std::erase_if(replacement.components, [](const Component& c) { return c.type == kImportSettingsComponent; });
                ComponentId componentId = 1;
                for (const Component& component : replacement.components) componentId = std::max(componentId, component.id + 1);
                replacement.components.push_back(make_import_settings_component(settings, componentId));
                command->add(std::make_unique<RemoveObjectsCommand>(
                    collect_editor_object_subtree_ids(document, std::vector<EditorObjectId>{groupId}), "Remove old parts"));
                command->add(std::make_unique<AddObjectCommand>(std::move(replacement), "Reimported group"));
            } else {
                EditorObject group(groupId, "Imported " + relativeSource.stem().string());
                group.sourceAsset = relativeSource;
                if (dropPoint) {
                    // Dragged in from Assets: the model's bottom centre lands where it was dropped.
                    // The group moves with its parts, so Reimport keeps the place.
                    EditorObjectBounds bounds;
                    for (const EditorObject& part : result->parts) bounds = union_bounds(bounds, object_world_bounds(part));
                    const Float3 offset = bottom_centre_offset(bounds, *dropPoint);
                    group.transform.position = offset;
                    for (EditorObject& part : result->parts) part.transform.position = add(part.transform.position, offset);
                }
                group.components.push_back(make_import_settings_component(settings, 1));
                command->add(std::make_unique<AddObjectCommand>(std::move(group), "Import group"));
            }
            std::vector<EditorObjectId> added{groupId};
            const std::size_t partCount = result->parts.size();
            for (EditorObject& part : result->parts) {
                // Parts sit where the group is: a moved group keeps its place on reimport.
                if (reimport) part.transform = compose_rigid_transforms(placement, part.transform);
                added.push_back(part.id);
                command->add(std::make_unique<AddObjectCommand>(std::move(part), "Imported part"));
            }
            result->parts.clear();
            const CommandResult executed = workspace_.commands().execute(document, std::move(command));
            if (!executed.success) { set_status(label + " failed: " + executed.message, true, 8.0F); return false; }
            if (!viewportSettings_.isolatedObjects.empty() && (!reimport || viewportSettings_.isolatedObjects.contains(groupId)))
                viewportSettings_.isolatedObjects.insert(added.begin(), added.end());
            for (const std::string& warning : result->warnings)
                workspace_.log().add(EditorLogLevel::Warning, label + ": " + warning);
            workspace_.select_object(groupId);
            if (!reimport && !dropPoint) frame_selection();
            recompute_layout();
            refresh_menu_state();
            const std::size_t warnings = result->warnings.size();
            set_status(std::string(reimport ? "Reimported " : "Imported ") + fileName + ": " + std::to_string(partCount) +
                       " object(s), " + format_thousands(result->voxels) + " voxels @ " + format_meters(result->voxelSize) +
                       (warnings > 0 ? ", " + std::to_string(warnings) + " warning(s) in Console" : std::string()) +
                       "; Undo " + (reimport ? "restores the previous parts" : "removes it"), warnings > 0, 10.0F);
            return true;
        });
}

// --- Drag assets from the Assets panel into the viewport (ART-076) -----------------------------


void NativeEditorController::begin_asset_drag(const EditorAssetRecord& asset, int x, int y) {
    assetDrag_ = {};
    assetDrag_.armed = true;
    assetDrag_.assetId = asset.id;
    assetDrag_.assetName = asset.displayName.empty() ? asset.relativePath.filename().string() : asset.displayName;
    assetDrag_.relativePath = asset.relativePath;
    assetDrag_.kind = asset.kind;
    assetDrag_.startX = x;
    assetDrag_.startY = y;
}

void NativeEditorController::cancel_asset_drag(std::string reason) {
    const bool wasActive = assetDrag_.active;
    assetDrag_ = {};
    if (wasActive && !reason.empty()) set_status(std::move(reason));
}

bool NativeEditorController::load_asset_drag_payload() {
    AssetDragState& drag = assetDrag_;
    const std::filesystem::path path = projectRoot_ / drag.relativePath;
    switch (drag.kind) {
    case EditorAssetKind::Prefab: {
        std::string error;
        auto prefab = load_editor_prefab(path, &error);
        if (!prefab) { drag.message = "Cannot place " + drag.assetName + ": " + (error.empty() ? "the prefab did not load" : error); return false; }
        for (const auto& [id, object] : prefab->templateDocument.objects())
            if (object.flags.visible) drag.localBounds = union_bounds(drag.localBounds, object_world_bounds(object));
        drag.prefab = std::make_shared<EditorPrefabAsset>(std::move(*prefab));
        return true;
    }
    case EditorAssetKind::Voxel: {
        if (path.extension() != ".dvox") { drag.message = drag.assetName + " is not a .dvox voxel asset"; return false; }
        DvoxReadResult loaded = read_dvox(path);
        if (!loaded.success) { drag.message = "Cannot place " + drag.assetName + ": " + loaded.error; return false; }
        if (loaded.asset.object.occupied_voxel_count() == 0) { drag.message = drag.assetName + " has no voxels"; return false; }
        auto object = std::make_shared<EditorObject>(EditorObjectId{1}, drag.relativePath.stem().string());
        object->voxelSizeMeters = loaded.asset.voxelSizeMeters;
        object->voxelScale = loaded.asset.voxelScale;
        object->voxels = voxel_copy_with_id(loaded.asset.object, 1);
        object->sourceAsset = drag.relativePath;
        drag.localBounds = object_world_bounds(*object);
        drag.voxelObject = std::move(object);
        return true;
    }
    case EditorAssetKind::PolygonMesh:
        if (!importable_model(drag.relativePath)) {
            drag.message = "Only OBJ, glTF and GLB models import; " + drag.assetName + " cannot be placed";
            return false;
        }
        return true;
    case EditorAssetKind::Material: {
        const std::string wanted = normalized_material_name(drag.relativePath.stem().string());
        for (const EditorMaterialEntry& entry : materials_.entries())
            if (normalized_material_name(entry.definition.name) == wanted) { drag.material = entry.id; return true; }
        drag.message = "No material named \"" + drag.relativePath.stem().string() +
                       "\" in this project's Material Library, so there is nothing to paint with";
        return false;
    }
    default:
        drag.message = "Drag prefabs, voxel assets (.dvox), models or materials into the viewport";
        return false;
    }
}

std::string NativeEditorController::material_drop_refusal(const EditorObject& target) const {
    if (!target.voxels || target.polygon || target.text3d || target.gaborVolume || target.voxels->occupied_voxel_count() == 0)
        return target.name + " has no voxels to paint (materials paint voxel objects)";
    if (!target.flags.visible || is_isolated_out(target.id)) return target.name + " is hidden or isolated out of view";
    if (target.flags.locked) return "Unlock " + target.name + " before painting it";
    if (target.prefabLink) return target.name + " is part of a prefab instance; unpack it or edit the prefab";
    return {};
}

std::string NativeEditorController::asset_drop_blocked_reason() const {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) return "Stop Play or Simulate before placing assets";
    if (longJob_) return long_job_busy_reason();
    return {};
}

void NativeEditorController::update_asset_drag(int x, int y) {
    AssetDragState& drag = assetDrag_;
    if (!drag.active) {
        const int dx = x - drag.startX, dy = y - drag.startY;
        if (dx * dx + dy * dy < 36) return;
        drag.active = true;
        drag.refused = !load_asset_drag_payload();
    }
    drag.overViewport = layout_.viewport.contains(x, y);
    drag.point.reset();
    drag.target.reset();
    drag.ghost = {};
    if (drag.refused) { set_status(drag.message, true, 2.0F); return; }
    if (!drag.overViewport) {
        drag.message = "Drag " + drag.assetName + " into the viewport (Esc cancels)";
        set_status(drag.message, false, 2.0F);
        return;
    }
    const ViewportRay ray = make_viewport_ray(camera_, layout_.viewport, static_cast<float>(x), static_cast<float>(y));
    if (const auto hit = pick_editor_document(workspace_.document(), ray, 10000.0F, &viewportSettings_.isolatedObjects)) {
        drag.point = hit->worldPosition;
        drag.target = hit->objectId;
    } else if (ray.direction.y < -1.0e-4F) {
        // Nothing under the cursor: land on the ground plane (y = 0) in front of the camera.
        const float t = -ray.origin.y / ray.direction.y;
        if (t > 0.0F && t < 10000.0F) drag.point = add(ray.origin, multiply(ray.direction, t));
    }
    const EditorObject* target = drag.target ? workspace_.document().find_object(*drag.target) : nullptr;
    if (const std::string blocked = asset_drop_blocked_reason(); !blocked.empty()) {
        drag.message = blocked;  // a release here would do nothing: never show a ghost that cannot apply
        drag.ghost = {};
        set_status(drag.message, true, 2.0F);
        return;
    }
    if (drag.kind == EditorAssetKind::Material) {
        if (!target) drag.message = "Drop " + drag.assetName + " on a voxel object to paint it";
        else if (const std::string refusal = material_drop_refusal(*target); !refusal.empty()) drag.message = "Cannot paint: " + refusal;
        else {
            drag.ghost = object_world_bounds(*target);
            const bool limited = !voxelSelection_.empty() && voxelSelectionObject_ == target->id;
            drag.message = "Release to paint " + std::string(limited ? "the selected voxels of " : "") + target->name +
                           " with " + drag.assetName + " (Esc cancels)";
        }
    } else if (!drag.point) {
        drag.message = "Point at a surface or the ground to place " + drag.assetName;
    } else {
        if (drag.localBounds.valid) drag.ghost = offset_bounds(drag.localBounds, bottom_centre_offset(drag.localBounds, *drag.point));
        drag.message = (drag.kind == EditorAssetKind::PolygonMesh ? "Release to import " : "Release to place ") + drag.assetName +
                       (target ? " on " + target->name : std::string(" on the ground")) + " (Esc cancels)";
    }
    set_status(drag.message, false, 2.0F);
}

void NativeEditorController::finish_asset_drag(int x, int y) {
    if (!assetDrag_.active) { assetDrag_ = {}; return; }
    update_asset_drag(x, y);
    if (!assetDrag_.overViewport) { cancel_asset_drag("Drag cancelled: released outside the viewport"); return; }
    (void)place_dragged_asset();
    assetDrag_ = {};
}

bool NativeEditorController::place_dragged_asset() {
    AssetDragState& drag = assetDrag_;
    if (drag.refused) { set_status(drag.message, true, 6.0F); return false; }
    if (const std::string blocked = asset_drop_blocked_reason(); !blocked.empty()) { set_status(blocked, true); return false; }
    EditorDocument& document = workspace_.document();
    const auto beforeSelection = capture_selection_snapshot();

    if (drag.kind == EditorAssetKind::Material) {
        const EditorObject* target = drag.target ? document.find_object(*drag.target) : nullptr;
        if (!target) { set_status("Drop the material on a voxel object to paint it", true); return false; }
        if (const std::string refusal = material_drop_refusal(*target); !refusal.empty()) { set_status(refusal, true); return false; }
        const MaterialId to = *drag.material;
        prune_voxel_selection();
        const bool limited = !voxelSelection_.empty() && voxelSelectionObject_ == target->id;
        const std::set<Int3> cells = limited ? voxelSelection_ : all_occupied_voxels(*target->voxels);
        if (cells.size() > kMaxFloodFillVoxels) {
            set_status(target->name + " has more than 2,000,000 voxels; select a region (M) and drop on it", true, 6.0F);
            return false;
        }
        std::vector<VoxelBooleanChange> changes;
        for (const Int3 cell : cells) {
            const MaterialId from = target->voxels->material_at(cell);
            if (from != kAirMaterial && from != to) changes.push_back({cell, from, to});
        }
        if (changes.empty()) { set_status(target->name + " already uses " + drag.assetName, true); return false; }
        const std::vector<Int3> anchors(target->anchors.begin(), target->anchors.end());
        const std::size_t count = changes.size();
        const std::string name = target->name;
        const CommandResult executed = workspace_.commands().execute(document,
            std::make_unique<ApplyVoxelBooleanCommand>(target->id, std::move(changes), anchors, anchors, "Paint with " + drag.assetName));
        if (!executed.success) { set_status(executed.message, true); return false; }
        set_status("Painted " + format_thousands(count) + " voxels of " + name + " with " + drag.assetName + "; Undo restores");
        return true;
    }

    if (!drag.point) { set_status("Point at a surface or the ground to place " + drag.assetName, true); return false; }
    const Float3 point = *drag.point;

    if (drag.kind == EditorAssetKind::PolygonMesh) {
        // Models go through the import options; the result lands with its bottom centre here.
        assetBrowserState_.selectedId = drag.assetId;
        refresh_menu_state();
        pendingImportDrop_ = point;
        if (!dispatch_action("file.import")) { pendingImportDrop_.reset(); return false; }
        return true;
    }

    if (drag.kind == EditorAssetKind::Prefab && drag.prefab) {
        const Float3 offset = bottom_centre_offset(drag.localBounds, point);
        auto command = std::make_unique<InstantiateEditorPrefabCommand>(std::move(*drag.prefab), make_rigid_transform(offset, {}),
                                                                        "Place " + drag.assetName);
        drag.prefab.reset();
        auto* placed = command.get();
        const CommandResult executed = workspace_.commands().execute(document, std::move(command));
        if (!executed.success) { set_status(executed.message, true); return false; }
        workspace_.clear_selection();
        for (const EditorObjectId root : placed->result().rootObjectIds) workspace_.add_to_selection(root);
        if (!viewportSettings_.isolatedObjects.empty())
            viewportSettings_.isolatedObjects.insert(placed->result().objectIds.begin(), placed->result().objectIds.end());
        set_selection_domain(SelectionDomain::Objects);
        workspace_.commands().set_last_selection_transition(beforeSelection, capture_selection_snapshot());
        recompute_layout();
        set_status("Placed " + drag.assetName + " (" + std::to_string(placed->result().objectIds.size()) + " objects); Undo removes it");
        return true;
    }

    if (drag.kind == EditorAssetKind::Voxel && drag.voxelObject) {
        const EditorObjectId id = document.allocate_object_id();
        EditorObject object(id, drag.voxelObject->name);
        object.voxelSizeMeters = drag.voxelObject->voxelSizeMeters;
        object.voxelScale = drag.voxelObject->voxelScale;
        object.voxels = voxel_copy_with_id(*drag.voxelObject->voxels, id);
        object.sourceAsset = drag.voxelObject->sourceAsset;
        object.transform = make_rigid_transform(bottom_centre_offset(drag.localBounds, point), {});
        const std::string name = object.name;
        const CommandResult executed = workspace_.commands().execute(document,
            std::make_unique<AddObjectCommand>(std::move(object), "Place " + drag.assetName));
        if (!executed.success) { set_status(executed.message, true); return false; }
        set_selection_domain(SelectionDomain::Objects);
        workspace_.select_object(id);
        if (!viewportSettings_.isolatedObjects.empty()) viewportSettings_.isolatedObjects.insert(id);
        workspace_.commands().set_last_selection_transition(beforeSelection, capture_selection_snapshot());
        recompute_layout();
        set_status("Placed " + name + "; Undo removes it");
        return true;
    }
    set_status("Nothing to place", true);
    return false;
}

// --- Long jobs ---------------------------------------------------------------------------------

bool NativeEditorController::import_polygon_model(const std::filesystem::path& path) {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) {
        set_status("Stop Play or Simulate before importing.", true);
        return false;
    }
    // has_root_* rather than is_absolute(): a rooted path is never joined to the project on Windows.
    const auto absolute = (path.has_root_directory() || path.has_root_name()) ? path : projectRoot_ / path;
    std::string extension = path.extension().string();
    for (auto& ch : extension) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (!is_model_source(path) && extension != ".dmesh") {
        set_status("Polygon import reads OBJ, glTF, GLB and DMESH files.", true);
        return false;
    }
    close_previews_for_voxel_edit("Import started");
    auto mesh = std::make_shared<std::optional<CookedPolygonAsset>>();
    const auto id = workspace_.document().allocate_object_id();
    return start_long_job(LongJobKind::ImportModel, "Import polygon " + path.filename().string(), longJobThreshold_ + 1,
        [mesh, absolute, id, extension](LongJob& job) {
            const auto fail = [&job](std::string message) {
                std::lock_guard lock(job.mutex);
                job.error = std::move(message);
            };
            if (job.cancelled()) return;
            job.report(0.1F, "Reading polygon geometry and materials");
            if (extension == ".dmesh") {
                auto loaded = read_dmesh(absolute, 256ULL * 1024ULL * 1024ULL);
                if (!loaded) { fail(loaded.error); return; }
                *mesh = std::move(loaded.asset);
            } else {
                PolygonCookOptions options;
                options.objectId = id;
                options.maximumVertices = 1000000;
                options.maximumIndices = 6000000;
                *mesh = cook_polygon_model(absolute, {}, options);
            }
            if (job.cancelled()) return;
            // Keep the cooked attributes (authored tangents included); only the owner changes.
            (*mesh)->objectId = id;
            (*mesh)->contentHash = polygon_asset_content_hash(**mesh);
            const auto valid = validate_polygon_asset(**mesh);
            if (!valid) { fail(valid.message); return; }
            job.report(1.0F, "Polygon ready");
        },
        [this, mesh, path, id](LongJob& job) {
            if (!*mesh) { set_status(job.label + " produced no geometry", true, 8.0F); return false; }
            const auto before = capture_selection_snapshot();
            EditorObject object(id, path.stem().string());
            object.voxelSizeMeters = 1.0F;
            object.polygon = std::move(**mesh);
            object.sourceAsset = path;
            object.transform.position = camera_.target;
            const auto result = workspace_.commands().execute(
                workspace_.document(), std::make_unique<AddObjectCommand>(std::move(object), "Import polygon model"));
            if (!result.success) { set_status(result.message, true); return false; }
            if (!viewportSettings_.isolatedObjects.empty()) viewportSettings_.isolatedObjects.insert(id);
            workspace_.select_object(id);
            selectionDomain_ = SelectionDomain::Objects;
            workspace_.commands().set_last_selection_transition(before, capture_selection_snapshot());
            frame_selection();
            refresh_menu_state();
            set_status("Imported " + path.filename().string() + " as an owned polygon mesh; the source file is unchanged");
            return true;
        });
}

// Exact Boolean through brepkernel. Prepared here (copies what the tool needs), run on the
// long-job worker, applied on completion as one undo step: a new "<A> (Exact Boolean)" object
// carrying the re-meshed result and its exact source. The operands stay as they are. A
// refusal changes nothing; when Manifold is built in, its uncertified mesh preview opens so
// the user can still choose a mesh-only result (Enter) or cancel (Esc).
bool NativeEditorController::start_exact_boolean(EditorObjectId a,EditorObjectId b,MeshBoolean op){
    auto request=prepare_exact_boolean(projectRoot_,workspace_.document(),a,b,op);
    if(!request.error.empty()){set_status(request.error,true);return false;}
    auto output=std::make_shared<std::optional<ExactBooleanResult>>();
    const auto root=projectRoot_;
    const auto settings=ExactCadSettings::from_environment();
    const char* label=op==MeshBoolean::Union?"union":op==MeshBoolean::Difference?"A minus B":op==MeshBoolean::ReverseDifference?"B minus A":"intersection";
    return start_long_job(LongJobKind::ImportModel,std::string("Exact Boolean ")+label,longJobThreshold_+1,
        [output,request,root,settings](LongJob& job){
            if(job.cancelled())return;
            job.report(.1F,"brepkernel is computing and certifying the exact result");
            auto result=run_exact_boolean(root,request,settings);
            if(job.cancelled())return;
            *output=std::move(result);
            job.report(1.F,"Exact Boolean finished");
        },
        [this,output,request](LongJob& job){
            if(!*output){set_status(job.label+" produced no result",true,8.F);return false;}
            auto& r=**output;
            auto& document=workspace_.document();
            if(exact_operand_stamp(document,request.a,request.b)!=request.operandStamp){
                set_status(job.label+" was not applied: an operand changed while it ran. Run it again",true,8.F);return false;}
            const auto* objectA=document.find_object(request.a);
            if(!objectA){set_status(job.label+" was not applied: operand A is gone",true,8.F);return false;}
            if(r.refused){
                std::string message=r.edit.error;if(!r.advisory.empty())message+=". "+r.advisory;
                if(open_mesh_boolean_preview(request.a,request.b,request.operation))
                    message+=". Showing the uncertified Manifold mesh preview instead: Enter applies a mesh-only result (no exact source), Esc cancels";
                set_status(message,true,12.F);return false;
            }
            if(!r){set_status(r.edit.error,true,8.F);return false;}
            const auto before=capture_selection_snapshot();
            const auto id=document.allocate_object_id();
            EditorObject result(id,objectA->name+" (Exact Boolean)");
            result.polygon=std::move(r.edit.mesh);result.polygon->objectId=id;refresh_editor_polygon(*result.polygon);
            // Same parent and local pose as A: the result is in A's mesh-local frame.
            result.transform=objectA->transform;result.parent=objectA->parent;result.voxelScale=objectA->voxelScale;
            result.voxelSizeMeters=objectA->voxelSizeMeters;result.flags=objectA->flags;
            Component exact=make_exact_brep_component(r.source);exact.id=1;result.components.push_back(std::move(exact));
            auto added=workspace_.commands().execute(document,std::make_unique<AddObjectCommand>(std::move(result),"Create exact Boolean result"));
            if(!added.success){set_status(added.message,true);return false;}
            if(!viewportSettings_.isolatedObjects.empty())viewportSettings_.isolatedObjects.insert(id);
            workspace_.select_object(id);selectionDomain_=SelectionDomain::Objects;
            workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot());
            recompute_layout();refresh_menu_state();
            set_status(r.summary+"; the operands are unchanged");
            return true;
        });
}

// Exact CAD: brepkernel turns the file into a project-owned .brep plus a meshed polygon; the
// object keeps the .brep as its dve.exact_brep source so exact Booleans can use it later.
bool NativeEditorController::import_exact_cad_model(const std::filesystem::path& path) {
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) {
        set_status("Stop Play or Simulate before importing.", true);
        return false;
    }
    if (!is_exact_cad_source(path)) {
        set_status("Exact CAD import reads STEP, BREP and OpenUSD B-rep (.usda) files.", true);
        return false;
    }
    const auto absolute = (path.has_root_directory() || path.has_root_name()) ? path : projectRoot_ / path;
    close_previews_for_voxel_edit("Import started");
    auto imported = std::make_shared<std::optional<ExactCadImportResult>>();
    const auto id = workspace_.document().allocate_object_id();
    const auto root = projectRoot_;
    const auto settings = ExactCadSettings::from_environment();
    return start_long_job(LongJobKind::ImportModel, "Import exact CAD " + path.filename().string(), longJobThreshold_ + 1,
        [imported, absolute, id, root, settings](LongJob& job) {
            if (job.cancelled()) return;
            job.report(0.1F, "Reading the B-rep and meshing it with brepkernel");
            auto result = import_exact_cad(root, absolute, id, settings);
            if (job.cancelled()) return;
            if (!result) {
                std::lock_guard lock(job.mutex);
                job.error = result.error;
                return;
            }
            *imported = std::move(result);
            job.report(1.0F, "Exact CAD ready");
        },
        [this, imported, path, id](LongJob& job) {
            if (!*imported) { set_status(job.label + " produced no geometry", true, 8.0F); return false; }
            auto& result = **imported;
            const auto before = capture_selection_snapshot();
            EditorObject object(id, path.stem().string());
            object.voxelSizeMeters = 1.0F;
            object.polygon = std::move(result.mesh);
            object.sourceAsset = path;
            object.transform.position = camera_.target;
            Component exact = make_exact_brep_component(result.source);
            exact.id = 1;                    // component ids are per object; this is the first
            object.components.push_back(std::move(exact));
            const auto added = workspace_.commands().execute(
                workspace_.document(), std::make_unique<AddObjectCommand>(std::move(object), "Import exact CAD"));
            if (!added.success) { set_status(added.message, true); return false; }
            if (!viewportSettings_.isolatedObjects.empty()) viewportSettings_.isolatedObjects.insert(id);
            workspace_.select_object(id);
            selectionDomain_ = SelectionDomain::Objects;
            workspace_.commands().set_last_selection_transition(before, capture_selection_snapshot());
            frame_selection();
            refresh_menu_state();
            set_status(result.summary + "; exact source kept at " + result.source.relativePath.generic_string());
            return true;
        });
}

bool NativeEditorController::start_long_job(LongJobKind kind, std::string label, std::uint64_t workUnits,
                                            std::function<void(LongJob&)> work, std::function<bool(LongJob&)> commit) {
    if (longJob_) { set_status(long_job_busy_reason(), true, 4.0F); return false; }
    auto job = std::make_shared<LongJob>();
    job->kind = kind;
    job->label = label;
    job->documentRevision = scene_change_fingerprint(workspace_.document(), workspace_.commands());
    job->commit = std::move(commit);
    if (workUnits <= longJobThreshold_) {
        // Small enough to finish at once: same work and commit, no progress UI.
        try { work(*job); } catch (const std::exception& exception) { job->error = exception.what(); }
        job->done = true;
        if (!job->error.empty()) { set_status(label + " failed: " + job->error, true, 8.0F); return false; }
        return job->commit(*job);
    }
    // Nothing may change the scene while the job runs: end any gesture first.
    finish_voxel_stroke();
    if (gizmoDragging_) finish_gizmo_drag(true);
    cancel_text_edit();
    close_top_level_menu();
    close_context_menu();
    commandPaletteOpen_ = false;
    floodFillArmed_ = false;
    job->report(0.0F, "Starting");
    std::string error;
    const auto task = longJobTasks_.submit(label, [job, work = std::move(work), wake = wakeCallback_](EditorTaskContext&) {
        try {
            // A job cancelled while it waited behind an earlier (cancelled) one does no work.
            if (!job->cancelled()) work(*job);
        } catch (const std::exception& exception) {
            std::lock_guard lock(job->mutex);
            job->error = exception.what();
        }
        job->done = true;
        if (wake) wake();
    }, &error);
    if (!task) { set_status(label + " could not start: " + error, true, 6.0F); return false; }
    longJob_ = std::move(job);
    refresh_menu_state();
    set_status(label + ": working (Esc cancels)", false, 0.0F);
    return true;
}

void NativeEditorController::poll_long_job() {
    if (longJob_ && longJob_->done.load()) finish_long_job();
}

void NativeEditorController::finish_long_job() {
    const std::shared_ptr<LongJob> job = std::move(longJob_);
    longJob_.reset();
    if (!job) return;
    std::string error;
    {
        std::lock_guard lock(job->mutex);
        error = job->error;
    }
    if (job->cancelled()) set_status(job->label + " cancelled; the scene is unchanged");
    else if (!error.empty()) set_status(job->label + " failed: " + error, true, 8.0F);
    else if (job->documentRevision != scene_change_fingerprint(workspace_.document(), workspace_.commands()))
        set_status(job->label + " was not applied: the scene changed while it ran. Run it again", true, 8.0F);
    else (void)job->commit(*job);
    refresh_menu_state();
}

bool NativeEditorController::cancel_long_job() {
    if (!longJob_) return false;
    // The worker stops at its next check; its result is never applied, so nothing changed.
    longJob_->cancel = true;
    const std::string label = longJob_->label;
    longJob_.reset();
    cancel_voxel_authoring_preview();
    refresh_menu_state();
    set_status(label + " cancelled; the scene is unchanged");
    return true;
}

bool NativeEditorController::wait_for_long_job(double timeoutSeconds) {
    if (!longJob_) return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeoutSeconds);
    while (!longJob_->done.load()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    finish_long_job();
    return true;
}

LongJobView NativeEditorController::long_job_view() const {
    LongJobView view;
    if (!longJob_) return view;
    view.active = true;
    view.kind = longJob_->kind;
    view.label = longJob_->label;
    view.progress = longJob_->progress.load();
    view.cancelling = longJob_->cancelled();
    std::lock_guard lock(longJob_->mutex);
    view.phase = longJob_->phase;
    return view;
}

NativeLongJobLayout NativeEditorController::long_job_layout() const {
    NativeLongJobLayout result;
    const UiRect area = layout_.viewport.width > 0 ? layout_.viewport : UiRect{0, 0, width_, height_};
    result.panel.width = std::max(260, std::min(560, area.width - 32));
    result.panel.height = 78;
    result.panel.x = area.x + (area.width - result.panel.width) / 2;
    result.panel.y = area.y + area.height - result.panel.height - 18;
    result.cancelButton = {result.panel.x + result.panel.width - 12 - 84, result.panel.y + 10, 84, 26};
    result.bar = {result.panel.x + 12, result.panel.y + result.panel.height - 26, result.panel.width - 24, 14};
    return result;
}

std::string NativeEditorController::long_job_busy_reason() const {
    if (!longJob_) return {};
    return "Busy: " + longJob_->label + " is running. Wait for it, or press Esc to cancel";
}

// --- Voxel selection (ART-043) -----------------------------------------------------------------

void NativeEditorController::set_selection_domain(SelectionDomain domain) {
    if (selectionDomain_ == domain) return;
    cancel_polygon_preview();
    if (gizmoDragging_) finish_gizmo_drag(true);
    if (domain == SelectionDomain::Polygons) {
        voxelSelectionBrushEnabled_ = false;
        (void)set_polygon_mode(PolygonSelectionMode::Face);
        return;
    }
    selectionDomain_ = domain;
    voxelBrushSelecting_ = false;
    // The voxel Selection Brush is the voxel domain's Paint shape: keep the two in step.
    voxelSelectionBrushEnabled_ = domain == SelectionDomain::Voxels && selectionShape_ == SelectionShape::Paint;
    voxelMoveAxis_ = 0;
    marquee_ = {};
    if (domain == SelectionDomain::Voxels) {
        const auto active = workspace_.selected_object();
        const EditorObject* object = active ? workspace_.document().find_object(*active) : nullptr;
        if (object && object->voxels && !object->polygon && !object->text3d && !object->gaborVolume) {
            if (voxelSelectionObject_ != object->id) {
                voxelSelection_.clear();
                ++voxelSelectionSerial_;
            }
            voxelSelectionObject_ = object->id;
            voxelSelectionRevision_ = object->voxels->revision();
        }
        set_status("Selecting voxels" + (object ? " of " + object->name : std::string()) +
                   ": click a piece; left-to-right box contains, right-to-left crosses; Shift adds, Ctrl removes, Ctrl+Shift intersects (M returns to objects)");
    } else {
        set_status("Selecting objects");
    }
    refresh_menu_state();
}

void NativeEditorController::clear_voxel_selection() {
    voxelSelection_.clear();
    ++voxelSelectionSerial_;
    refresh_menu_state();
}

void NativeEditorController::select_voxels(EditorObjectId objectId, const std::set<Int3>& cells, SelectionOperation operation) {
    const EditorObject* object = workspace_.document().find_object(objectId);
    if (!object || !object->voxels || object->polygon || object->text3d || object->gaborVolume) return;
    if (!object->flags.visible || is_isolated_out(objectId)) {
        set_status(object->name + " is hidden or isolated out of view: show it before selecting its voxels", true);
        return;
    }
    // An over-budget search may have stopped early: never apply its partial result, even for
    // subtraction or intersection, whose final selection would otherwise fit the budget.
    if (cells.size() > kMaxVoxelSelection) {
        set_status("That box or connected piece exceeds 500000 voxels; the selection is unchanged",true,6.0F);
        return;
    }
    // Cap: refuse (and keep the current selection) rather than build a huge set.
    {
        const bool sameObject = objectId == voxelSelectionObject_;
        std::size_t projected = cells.size();
        if (sameObject && (operation == SelectionOperation::Add || operation == SelectionOperation::Toggle))
            projected += voxelSelection_.size();
        else if (sameObject && (operation == SelectionOperation::Subtract || operation == SelectionOperation::Intersect))
            projected = 0;
        if (projected > kMaxVoxelSelection) {
            std::size_t result = projected;
            if (sameObject && (operation == SelectionOperation::Add || operation == SelectionOperation::Toggle)) {
                result = voxelSelection_.size();
                for (const Int3 cell : cells) {
                    if (!voxelSelection_.contains(cell)) ++result;
                    else if (operation == SelectionOperation::Toggle) --result;
                }
            }
            if (result > kMaxVoxelSelection) {
                set_status("That would select more than " + std::to_string(kMaxVoxelSelection) +
                           " voxels; the selection is unchanged (use Separate Islands or Slice for whole-object work)", true, 6.0F);
                return;
            }
        }
    }
    if (objectId != voxelSelectionObject_) {
        // One object at a time: picking voxels of another object starts over there.
        voxelSelection_.clear();
        ++voxelSelectionSerial_;
        voxelSelectionObject_ = objectId;
        if (operation == SelectionOperation::Subtract || operation == SelectionOperation::Intersect) operation = SelectionOperation::Replace;
        workspace_.select_object(objectId);
    }
    voxelSelectionRevision_ = object->voxels->revision();
    switch (operation) {
        case SelectionOperation::Replace: voxelSelection_ = cells; break;
        case SelectionOperation::Add: voxelSelection_.insert(cells.begin(), cells.end()); break;
        case SelectionOperation::Subtract: for (const Int3 cell : cells) voxelSelection_.erase(cell); break;
        case SelectionOperation::Toggle:
            for (const Int3 cell : cells) if (!voxelSelection_.erase(cell)) voxelSelection_.insert(cell);
            break;
        case SelectionOperation::Intersect: {
            std::set<Int3> kept;
            for (const Int3 cell : cells) if (voxelSelection_.contains(cell)) kept.insert(cell);
            voxelSelection_ = std::move(kept);
            break;
        }
    }
    ++voxelSelectionSerial_;
    refresh_menu_state();
    set_status(std::to_string(voxelSelection_.size()) + " voxel(s) selected in " + object->name, false, 3.0F);
}

void NativeEditorController::prune_voxel_selection() {
    if (voxelSelection_.empty() && voxelSelectionObject_ == 0) return;
    const EditorObject* object = workspace_.document().find_object(voxelSelectionObject_);
    if (!object || !object->voxels) {
        voxelSelection_.clear();
        ++voxelSelectionSerial_;
        voxelSelectionObject_ = 0;
        return;
    }
    if (!voxelSelection_.empty() && (!object->flags.visible || is_isolated_out(object->id))) {
        // Never keep (and let Delete or Move act on) voxels the artist cannot see.
        voxelSelection_.clear();
        ++voxelSelectionSerial_;
        voxelMoveAxis_ = 0;
        voxelMoveSteps_ = 0;
        set_status("Voxel selection cleared: " + object->name + " is hidden or isolated out of view", false, 4.0F);
        refresh_menu_state();
        return;
    }
    if (object->voxels->revision() == voxelSelectionRevision_) return;
    voxelSelectionRevision_ = object->voxels->revision();
    if (std::erase_if(voxelSelection_, [&](Int3 cell) { return !object->voxels->occupied_at(cell); }) > 0) {
        ++voxelSelectionSerial_;
        refresh_menu_state();
    }
}

EditorSelectionSnapshot NativeEditorController::capture_selection_snapshot() const {
    EditorSelectionSnapshot snapshot{workspace_.selected_objects(), workspace_.selected_object(), selectionDomain_ == SelectionDomain::Voxels,
            voxelSelectionObject_, voxelSelection_, selectionDomain_ == SelectionDomain::Polygons,
            polygonSelection_.object, static_cast<std::uint8_t>(polygonSelection_.mode), polygonSelection_.components,false,{}};
    const auto* object=workspace_.document().find_object(polygonSelection_.object);
    if(snapshot.polygonDomain && object && object->polygon && object->modelingTopology) {
        snapshot.polygonPersistent=true;snapshot.polygonIdentities=modeling_selection_ids(*object->polygon,*object->modelingTopology,polygonSelection_);
    }
    return snapshot;
}

void NativeEditorController::restore_selection_snapshot(const EditorSelectionSnapshot& snapshot) {
    workspace_.clear_selection();
    for (const auto id : snapshot.objects) {
        const auto* object = workspace_.document().find_object(id);
        if (object && object->flags.visible && !is_isolated_out(id)) workspace_.add_to_selection(id);
    }
    if (snapshot.primary && workspace_.is_selected(*snapshot.primary)) workspace_.add_to_selection(*snapshot.primary);
    reset_voxel_selection_state();
    selectionDomain_ = snapshot.polygonDomain ? SelectionDomain::Polygons : snapshot.voxelDomain ? SelectionDomain::Voxels : SelectionDomain::Objects;
    polygonSelection_ = {snapshot.polygonObject, static_cast<PolygonSelectionMode>(snapshot.polygonMode), snapshot.polygonComponents};
    if(snapshot.polygonPersistent) {
        polygonSelection_.components.clear();
        const auto* object=workspace_.document().find_object(snapshot.polygonObject);
        if(object&&object->polygon&&object->modelingTopology)polygonSelection_.components=resolve_modeling_selection(
            *object->polygon,*object->modelingTopology,snapshot.polygonMode,snapshot.polygonIdentities);
    }
    if (snapshot.polygonDomain) prune_polygon_selection();
    if (snapshot.voxelDomain) {
        const auto* object = workspace_.document().find_object(snapshot.voxelObject);
        if (object && object->voxels && object->flags.visible && !is_isolated_out(object->id)) {
            voxelSelectionObject_ = object->id;
            for (const auto cell : snapshot.voxels)
                if (object->voxels->occupied_at(cell)) voxelSelection_.insert(cell);
            voxelSelectionRevision_ = object->voxels->revision();
            ++voxelSelectionSerial_;
        }
    }
}

void NativeEditorController::reset_voxel_selection_state() {
    cancel_voxel_authoring_preview();
    voxelSelection_.clear();
    ++voxelSelectionSerial_;
    voxelSelectionObject_ = 0;
    voxelSelectionRevision_ = 0;
    voxelBrushSelecting_ = false;
    voxelSelectionBrushEnabled_ = selectionDomain_ == SelectionDomain::Voxels && selectionShape_ == SelectionShape::Paint;
    voxelRegionRotationPivot_.reset();
    voxelRegionRotationObject_ = 0;
    voxelRegionRotationSerial_ = 0;
    voxelMoveAxis_ = 0;
    voxelMoveSteps_ = 0;
    marquee_.clickVoxel.reset();
}

void NativeEditorController::close_previews_for_voxel_edit(std::string_view what) {
    cancel_voxel_authoring_preview();
    cancel_polygon_preview();
    // One preview at a time (#98): a voxel edit closes any open Slice / Boolean / Join / Scatter
    // preview first, so the edit cannot change an object under a preview.
    const std::string reason = std::string(what);
    cancel_voxel_slice();
    if (voxelBoolean_.active()) cancel_voxel_boolean("Boolean preview closed: " + reason);
    cancel_scatter("Scatter preview closed: " + reason);
    cancel_voxel_join("Join preview closed: " + reason);
}

Float3 NativeEditorController::voxel_selection_centre() const {
    const EditorObject* object = workspace_.document().find_object(voxelSelectionObject_);
    if (!object || voxelSelection_.empty()) return selection_pivot();
    // The gizmo asks for this several times a frame; walk the set only when it changed.
    if (voxelCentreCacheSerial_ != voxelSelectionSerial_ || !voxelCentreCacheValid_) {
        double sx = 0.0, sy = 0.0, sz = 0.0;
        for (const Int3 cell : voxelSelection_) {
            sx += static_cast<double>(cell.x) + 0.5;
            sy += static_cast<double>(cell.y) + 0.5;
            sz += static_cast<double>(cell.z) + 0.5;
        }
        const double n = static_cast<double>(voxelSelection_.size());
        voxelCentreCacheLocal_ = {static_cast<float>(sx / n), static_cast<float>(sy / n), static_cast<float>(sz / n)};
        voxelCentreCacheSerial_ = voxelSelectionSerial_;
        voxelCentreCacheValid_ = true;
    }
    return voxel_to_world(object->transform, multiply(object->voxelScale,object->voxelSizeMeters),voxelCentreCacheLocal_);
}

Float3 NativeEditorController::voxel_move_axis_world(int axis) const {
    // Voxels move along their object's own grid axes.
    const EditorObject* object = workspace_.document().find_object(voxelSelectionObject_);
    const Float3 local = axis == 1 ? Float3{1, 0, 0} : axis == 2 ? Float3{0, 1, 0} : Float3{0, 0, 1};
    return object ? rotate(object->transform.rotation, local) : local;
}

std::optional<Int3> NativeEditorController::voxel_move_preview() const noexcept {
    if (voxelMoveAxis_ == 0) return std::nullopt;
    return Int3{voxelMoveAxis_ == 1 ? voxelMoveSteps_ : 0, voxelMoveAxis_ == 2 ? voxelMoveSteps_ : 0,
                voxelMoveAxis_ == 3 ? voxelMoveSteps_ : 0};
}

std::size_t NativeEditorController::selection_fingerprint() const noexcept {
    // Order-independent hash of the voxel selection, so a different selection of the same size
    // does not reuse a stale brush preview.
    std::uint64_t h = voxelSelection_.size() * 0x9E3779B97F4A7C15ULL;
    for (const Int3& c : voxelSelection_) {
        std::uint64_t v = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.x)) * 0x100000001B3ULL) ^
                          (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.y)) << 21) ^
                          (static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.z)) << 42);
        v ^= v >> 33; v *= 0xFF51AFD7ED558CCDULL; v ^= v >> 33;
        h += v;
    }
    return static_cast<std::size_t>(h);
}

bool NativeEditorController::in_voxel_brush(Int3 cell, Int3 center) const noexcept {
    const std::int64_t dx = cell.x - center.x, dy = cell.y - center.y, dz = cell.z - center.z;
    const std::int64_t r = voxelBrushRadius_;
    if (std::abs(dx) > r || std::abs(dy) > r || std::abs(dz) > r) return false;
    return voxelBrushShape_ == BrushShape::Cube || dx * dx + dy * dy + dz * dz <= r * r;
}

std::optional<VoxelBrushPreview> NativeEditorController::voxel_brush_preview() const {
    const bool brushTool = activeTool_ == EditorToolId::AddVoxel || activeTool_ == EditorToolId::RemoveVoxel ||
                           activeTool_ == EditorToolId::PaintMaterial;
    if (!brushTool || !hoverPick_ || workspace_.mode() != EditorMode::Edit || playSession_.active() || gizmoDragging_)
        return std::nullopt;
    const EditorObject* object = workspace_.document().find_object(hoverPick_->objectId);
    if (!object || !object->voxels || object->polygon || object->text3d || object->gaborVolume) return std::nullopt;
    const bool limited = activeTool_ != EditorToolId::AddVoxel && !voxelSelection_.empty() && voxelSelectionObject_ == object->id;
    const BrushPreviewKey key{object->id, hoverPick_->voxel, hoverPick_->normal, voxelBrushRadius_, voxelBrushShape_,
                              activeTool_, activeMaterial_, object->voxels->revision(), limited ? selection_fingerprint() : 0U,
                              limited ? voxelSelectionObject_ : EditorObjectId{}, object->flags.locked};
    if (brushPreviewKey_ == key) return brushPreview_;
    VoxelBrushPreview preview;
    preview.objectId = object->id;
    preview.radius = voxelBrushRadius_;
    preview.shape = voxelBrushShape_;
    preview.limitedToSelection = limited;
    preview.locked = object->flags.locked;
    preview.operation = activeTool_ == EditorToolId::AddVoxel ? VoxelToolOperation::Add
        : activeTool_ == EditorToolId::RemoveVoxel ? VoxelToolOperation::Remove : VoxelToolOperation::Paint;
    preview.center = preview.operation == VoxelToolOperation::Add ? add_int3(hoverPick_->voxel, hoverPick_->normal) : hoverPick_->voxel;
    if (!preview.locked) {
        // The same rule as the brush command: Add fills empty cells, Remove empties occupied ones,
        // Paint changes occupied cells of another material; with a selection only selected cells.
        const auto shapeCells = voxelBrushShape_ == BrushShape::Sphere ? sphere_brush_voxels(preview.center, voxelBrushRadius_)
                                                                       : cube_brush_voxels(preview.center, voxelBrushRadius_);
        for (const Int3 cell : shapeCells) {
            const MaterialId before = object->voxels->material_at(cell);
            bool changes = false;
            // Mirrors changes_for(): Add writes the active material (or 1 for air) into every cell,
            // replacing other materials too; Paint with air changes nothing.
            const MaterialId addMaterial = activeMaterial_ == kAirMaterial ? MaterialId{1} : activeMaterial_;
            switch (preview.operation) {
            case VoxelToolOperation::Add: changes = before != addMaterial; break;
            case VoxelToolOperation::Remove: changes = before != kAirMaterial; break;
            case VoxelToolOperation::Paint: changes = before != kAirMaterial && activeMaterial_ != kAirMaterial && before != activeMaterial_; break;
            }
            if (changes && limited && !voxelSelection_.contains(cell)) changes = false;
            if (!changes) continue;
            ++preview.count;
            if (preview.cells.size() < kMaxBrushPreviewCells) preview.cells.push_back(cell);
        }
    }
    brushPreviewKey_ = key;
    brushPreview_ = std::move(preview);
    return brushPreview_;
}

void NativeEditorController::voxel_brush_select_at(int x, int y) {
    update_hover(x, y);
    if (!hoverPick_) return;
    const EditorObject* object = workspace_.document().find_object(hoverPick_->objectId);
    if (!object || !object->voxels) return;
    if (hoverPick_->objectId != voxelSelectionObject_ && voxelBrushOperation_ == SelectionOperation::Subtract) return;
    auto cells = voxels_in_cube(*object->voxels,hoverPick_->voxel,voxelBrushRadius_);
    std::erase_if(cells, [&](const Int3& cell) { return !in_voxel_brush(cell, hoverPick_->voxel); });
    if (!viewportSettings_.xraySelection) {
        std::size_t budget = kMaxVoxelSelectionRays;
        for (auto at = cells.begin(); at != cells.end();) {
            if (voxel_cell_visible(workspace_.document(),*object,*at,camera_,layout_.viewport,
                                   &viewportSettings_.isolatedObjects,budget)) ++at;
            else at = cells.erase(at);
            if (budget == 0) { set_status("Visible brush exceeds the ray budget; use a smaller radius or X-Ray",true); return; }
        }
    }
    select_voxels(hoverPick_->objectId,cells,voxelBrushOperation_);
}

void NativeEditorController::finish_voxel_marquee(int x, int y) {
    const MarqueeState marquee = marquee_;
    marquee_ = {};
    const int distanceSquared = (x - marquee.startX) * (x - marquee.startX) + (y - marquee.startY) * (y - marquee.startY);
    if (distanceSquared <= 25) {
        // Click: the clicked face-connected piece; on empty space a plain click clears.
        if (marquee.clickVoxel) {
            const EditorObject* object = workspace_.document().find_object(marquee.clickVoxel->objectId);
            if (object && object->voxels && !object->polygon) {
                const auto cells = voxelClickSelection_ == VoxelClickSelection::Cell ?
                    std::set<Int3>{marquee.clickVoxel->voxel} :
                    voxel_connected_piece(*object->voxels,marquee.clickVoxel->voxel,kMaxVoxelSelection);
                SelectionOperation operation = marquee.operation;
                // Shift+click on a selected piece takes it out again.
                if (operation == SelectionOperation::Add && object->id == voxelSelectionObject_ && !cells.empty() &&
                    std::all_of(cells.begin(), cells.end(), [&](const Int3& cell) { return voxelSelection_.contains(cell); }))
                    operation = SelectionOperation::Subtract;
                select_voxels(object->id,cells,operation);
            }
        } else if (marquee.operation == SelectionOperation::Replace) {
            clear_voxel_selection();
            set_status("Voxel selection cleared", false, 2.0F);
        }
        return;
    }
    EditorObjectId target = voxelSelectionObject_;
    if (!workspace_.document().find_object(target)) {
        const auto active = workspace_.selected_object();
        target = active ? *active : 0;
    }
    const EditorObject* object = workspace_.document().find_object(target);
    if (!object || !object->voxels || object->polygon || object->text3d || object->gaborVolume) {
        set_status("Box-select voxels of an object: click one of its voxels first", true);
        return;
    }
    const UiRect rect{std::min(marquee.startX, x), std::min(marquee.startY, y), std::abs(x - marquee.startX) + 1,
                      std::abs(y - marquee.startY) + 1};
    std::string error;
    const auto cells = voxels_in_screen_rect(*object,camera_,layout_.viewport,rect,viewportSettings_.xraySelection,
        x < marquee.startX,&workspace_.document(),&viewportSettings_.isolatedObjects,&error);
    if (!error.empty()) { set_status(error,true,6.0F); return; }
    select_voxels(target,cells,marquee.operation);
}

bool NativeEditorController::grow_voxel_selection_cmd() {
    const EditorObject* object = workspace_.document().find_object(voxelSelectionObject_);
    if (!object || voxelSelection_.empty()) { set_status("Select voxels first (press M)", true); return false; }
    select_voxels(object->id, grow_voxel_selection(*object->voxels, voxelSelection_, kMaxVoxelSelection), SelectionOperation::Replace);
    return true;
}
bool NativeEditorController::shrink_voxel_selection_cmd() {
    const EditorObject* object = workspace_.document().find_object(voxelSelectionObject_);
    if (!object || voxelSelection_.empty()) { set_status("Select voxels first (press M)", true); return false; }
    select_voxels(object->id, shrink_voxel_selection(voxelSelection_), SelectionOperation::Replace);
    return true;
}
bool NativeEditorController::invert_voxel_selection_cmd() {
    const EditorObject* object = workspace_.document().find_object(voxelSelectionObject_);
    if (!object || !object->voxels) { set_status("Pick a voxel object first (press M and click it)", true); return false; }
    select_voxels(object->id, invert_voxel_selection(*object->voxels, voxelSelection_, kMaxVoxelSelection), SelectionOperation::Replace);
    return true;
}
bool NativeEditorController::select_all_voxels() {
    EditorObjectId target = voxelSelectionObject_;
    if (!workspace_.document().find_object(target) && workspace_.selected_object()) target = *workspace_.selected_object();
    const EditorObject* object = workspace_.document().find_object(target);
    if (!object || !object->voxels || object->polygon || object->text3d || object->gaborVolume) { set_status("Select a voxel object first", true); return false; }
    if (object->voxels->occupied_voxel_count() > kMaxVoxelSelection) {
        set_status(object->name + " has more than " + std::to_string(kMaxVoxelSelection) +
                   " voxels; select a region instead (use Separate Islands or Slice for whole-object work)", true, 6.0F);
        return false;
    }
    select_voxels(object->id, all_occupied_voxels(*object->voxels), SelectionOperation::Replace);
    return voxelSelectionObject_ == object->id && !voxelSelection_.empty();
}

bool NativeEditorController::delete_selected_voxels() {
    const EditorObject* object = workspace_.document().find_object(voxelSelectionObject_);
    if (!object || voxelSelection_.empty()) { set_status("No voxels selected", true); return false; }
    std::string error;
    auto command = build_delete_voxels_command(*object, voxelSelection_, &error);
    if (!command) { set_status(error, true); return false; }
    close_previews_for_voxel_edit("voxels deleted");
    const std::size_t count = voxelSelection_.size();
    const CommandResult result = workspace_.commands().execute(workspace_.document(), std::move(command));
    if (!result.success) { set_status(result.message, true, 6.0F); return false; }
    voxelSelection_.clear();
    ++voxelSelectionSerial_;
    refresh_menu_state();
    set_status("Deleted " + std::to_string(count) + " voxel(s); Undo restores them");
    return true;
}

bool NativeEditorController::extract_selected_voxels() {
    if (voxelSelection_.empty()) { set_status("Select the voxels to extract first (press M)", true); return false; }
    ExtractVoxelsBuild build = build_extract_voxels_command(workspace_.document(), voxelSelectionObject_, voxelSelection_);
    if (!build.command) { set_status(build.error, true, 6.0F); return false; }
    close_previews_for_voxel_edit("voxels extracted");
    const std::size_t count = voxelSelection_.size();
    const auto beforeSelection = capture_selection_snapshot();
    const CommandResult result = workspace_.commands().execute(workspace_.document(), std::move(build.command));
    if (!result.success) { set_status(result.message, true, 6.0F); return false; }
    if (!viewportSettings_.isolatedObjects.empty() && viewportSettings_.isolatedObjects.contains(voxelSelectionObject_))
        viewportSettings_.isolatedObjects.insert(build.newObject);
    voxelSelection_.clear();
    ++voxelSelectionSerial_;
    selectionDomain_ = SelectionDomain::Objects;
    workspace_.select_object(build.newObject);
    workspace_.commands().set_last_selection_transition(beforeSelection, capture_selection_snapshot());
    recompute_layout();
    refresh_menu_state();
    const EditorObject* part = workspace_.document().find_object(build.newObject);
    set_status("Extracted " + std::to_string(count) + " voxel(s) to " + (part ? part->name : std::string("a new object")) +
               " (selected; nothing moved). Undo puts them back");
    return true;
}

bool NativeEditorController::copy_selected_voxels(bool cut) {
    const EditorObject* object = workspace_.document().find_object(voxelSelectionObject_);
    if (!object || voxelSelection_.empty()) { set_status("No voxels selected", true); return false; }
    if (cut) {
        // A Cut that cannot delete (locked object, ...) must not replace the clipboard either.
        std::string error;
        if (!build_delete_voxels_command(*object, voxelSelection_, &error)) { set_status(error, true); return false; }
    }
    voxelClipboard_ = copy_voxels(*object, voxelSelection_);
    voxelClipboardNewest_ = true;
    const std::size_t count = voxelClipboard_->cells.size();
    if (cut) {
        if (!delete_selected_voxels()) return false;
        set_status("Cut " + std::to_string(count) + " voxel(s); Paste makes a new object in place");
    } else {
        set_status("Copied " + std::to_string(count) + " voxel(s); Paste makes a new object in place");
    }
    refresh_menu_state();
    return true;
}

bool NativeEditorController::paste_voxels() {
    if (!voxelClipboard_ || voxelClipboard_->cells.empty()) { set_status("Nothing copied: copy voxels first", true); return false; }
    EditorDocument& document = workspace_.document();
    const EditorObjectId id = document.allocate_object_id();
    VoxelClipboard clipboard = *voxelClipboard_;
    if (clipboard.parent && !document.find_object(*clipboard.parent)) clipboard.parent.reset();
    close_previews_for_voxel_edit("voxels pasted");
    const CommandResult result = workspace_.commands().execute(document, std::make_unique<AddObjectCommand>(
        make_object_from_voxels(id, clipboard, clipboard.name + " (copy)"), "Paste voxels"));
    if (!result.success) { set_status(result.message, true, 6.0F); return false; }
    if (!viewportSettings_.isolatedObjects.empty()) viewportSettings_.isolatedObjects.insert(id);
    selectionDomain_ = SelectionDomain::Objects;
    voxelSelection_.clear();
    ++voxelSelectionSerial_;
    workspace_.select_object(id);
    recompute_layout();
    refresh_menu_state();
    set_status("Pasted " + std::to_string(clipboard.cells.size()) + " voxel(s) as a new object in place (selected: move it with W)");
    return true;
}

bool NativeEditorController::move_selected_voxels(Int3 offset) {
    const EditorObject* object = workspace_.document().find_object(voxelSelectionObject_);
    if (!object || voxelSelection_.empty()) { set_status("No voxels selected", true); return false; }
    std::string error;
    auto command = build_move_voxels_command(*object, voxelSelection_, offset, &error);
    if (!command) { set_status(error, true); return false; }
    const auto beforeSelection = capture_selection_snapshot();
    close_previews_for_voxel_edit("voxels moved");
    const CommandResult result = workspace_.commands().execute(workspace_.document(), std::move(command));
    if (!result.success) { set_status(result.message, true, 6.0F); return false; }
    std::set<Int3> moved;
    for (const Int3 cell : voxelSelection_) moved.insert({cell.x + offset.x, cell.y + offset.y, cell.z + offset.z});
    voxelSelection_ = std::move(moved);
    ++voxelSelectionSerial_;
    voxelSelectionRevision_ = object->voxels->revision();
    workspace_.commands().set_last_selection_transition(beforeSelection, capture_selection_snapshot());
    set_status("Moved " + std::to_string(voxelSelection_.size()) + " voxel(s) by (" + std::to_string(offset.x) + ", " +
               std::to_string(offset.y) + ", " + std::to_string(offset.z) + "); Undo puts them back");
    return true;
}

std::string NativeEditorController::voxel_region_disabled_reason() const {
    if (longJob_) return long_job_busy_reason();
    if (workspace_.mode() != EditorMode::Edit || playSession_.active()) return "Stop Play before editing voxels.";
    if (gizmoDragging_ || voxelMoveAxis_ != 0 || voxelStrokeActive_ || voxelBrushSelecting_ || marquee_.active)
        return "Finish the current drag or brush stroke first.";
    if (selectionDomain_ != SelectionDomain::Voxels || voxelSelection_.empty())
        return "Select a voxel region first: press M, then click or box-select cells.";
    const auto* object = workspace_.document().find_object(voxelSelectionObject_);
    if (!object || !object->voxels || object->polygon || object->text3d || object->gaborVolume)
        return "Select cells of a voxel object.";
    if (object->flags.locked) return "Unlock " + object->name + " first.";
    if (!object->flags.visible || is_isolated_out(object->id)) return "Show the voxel object before editing its cells.";
    return {};
}

bool NativeEditorController::edit_selected_voxel_region(VoxelRegionEdit edit, VoxelRegionSettings settings) {
    prune_voxel_selection();
    if (const auto reason = voxel_region_disabled_reason(); !reason.empty()) {
        set_status(reason,true); return false;
    }
    const auto id = voxelSelectionObject_;
    const auto* object = workspace_.document().find_object(id);
    // Keep the same snapped pivot for consecutive quarter turns. Re-snapping an even-width
    // region after every turn would drift by a cell when its rotated bounds change parity.
    if (edit == VoxelRegionEdit::RotateQuarterTurn && !settings.rotationPivot &&
        voxelRegionRotationObject_ == id && voxelRegionRotationSerial_ == voxelSelectionSerial_)
        settings.rotationPivot = voxelRegionRotationPivot_;
    auto built = build_voxel_region_command(*object,voxelSelection_,edit,settings);
    if (!built.command) { set_status(built.error,true,6.0F); return false; }
    const auto before = capture_selection_snapshot();
    close_previews_for_voxel_edit("selected voxel region edited");
    const auto result = workspace_.commands().execute(workspace_.document(),std::move(built.command));
    if (!result.success) { set_status(result.message,true,6.0F); return false; }
    voxelSelection_ = std::move(built.selection);
    ++voxelSelectionSerial_;
    voxelRegionRotationPivot_ = built.rotationPivot;
    voxelRegionRotationObject_ = id;
    voxelRegionRotationSerial_ = voxelSelectionSerial_;
    object = workspace_.document().find_object(id);
    voxelSelectionRevision_ = object->voxels->revision();
    workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot());
    recompute_layout();
    refresh_menu_state();
    const std::string operation = edit == VoxelRegionEdit::Extrude ? "Extruded" :
        edit == VoxelRegionEdit::RotateQuarterTurn ? "Rotated 90 degrees" : "Scaled by integer " + std::to_string(settings.amount);
    set_status(operation + ": " + std::to_string(voxelSelection_.size()) + " voxel(s) selected; Undo restores the region",false,5.0F);
    return true;
}

bool NativeEditorController::begin_voxel_slice() {
    floodFillArmed_ = false;  // another tool takes the next viewport click
    if (const auto reason = voxel_slice_disabled_reason(); !reason.empty()) {
        set_status(reason, true);
        return false;
    }
    cancel_voxel_boolean();
    cancel_scatter("Scatter preview closed: Slice started");
    cancel_voxel_join("Join preview closed: Slice started");
    cancel_text_edit();
    close_context_menu();
    if (gizmoDragging_)
        finish_gizmo_drag(true);
    finish_voxel_stroke();
    finish_slice_pointer(false);
    marquee_ = {};
    voxelMoveAxis_ = 0;
    voxelMoveSteps_ = 0;
    voxelBrushSelecting_ = false;
    const auto bounds = selection_bounds();
    std::string error;
    if (!voxelSlice_.begin(workspace_.document(), *workspace_.selected_object(),
                           multiply(add(bounds.minimum, bounds.maximum), 0.5F), {1, 0, 0}, &error)) {
        set_status(error, true);
        return false;
    }
    refresh_menu_state();
    set_status("Slice: drag yellow shaft to move, coloured rings to rotate; panel edits plane and output", false, 8);
    return true;
}
bool NativeEditorController::configure_voxel_slice(Float3 point, Float3 normal, VoxelSliceOutput output) {
    if (!voxelSlice_.active())
        return false;
    std::string error;
    const bool ok = voxelSlice_.configure(workspace_.document(), point, normal, output, &error);
    if (!ok)
        set_status(error, true);
    refresh_menu_state();
    return ok;
}
CommandResult NativeEditorController::commit_voxel_slice() {
    if (textEdit_.kind == TextEditKind::SlicePoint || textEdit_.kind == TextEditKind::SliceNormal) {
        commit_text_edit();
        if (textEdit_.kind != TextEditKind::Inactive) return CommandResult::fail(textEdit_.error);
    }
    if (const auto reason = voxel_slice_disabled_reason(); !reason.empty()) {
        set_status(reason, true);
        return CommandResult::fail(reason);
    }
    if (!voxelSlice_.active() || workspace_.selected_object() != voxelSlice_.target())
        return CommandResult::fail("Slice selection changed; reopen the preview.");
    finish_slice_pointer(false);
    const auto beforeSelection = capture_selection_snapshot();
    auto result = voxelSlice_.commit(workspace_);
    if (result.success) {
        prune_voxel_selection();
        workspace_.commands().set_last_selection_transition(beforeSelection, capture_selection_snapshot());
        if (textEdit_.kind == TextEditKind::SlicePoint || textEdit_.kind == TextEditKind::SliceNormal) cancel_text_edit();
    }
    recompute_layout();
    refresh_menu_state();
    set_status(result.success ? "Slice applied. Ctrl+Z restores both halves in one step." : result.message,
               !result.success, 8);
    return result;
}
void NativeEditorController::cancel_voxel_slice() {
    finish_slice_pointer(false);
    if (textEdit_.kind == TextEditKind::SlicePoint || textEdit_.kind == TextEditKind::SliceNormal) cancel_text_edit();
    if (!voxelSlice_.active())
        return;
    voxelSlice_.cancel();
    refresh_menu_state();
    set_status("Slice cancelled; scene unchanged");
}
bool NativeEditorController::handle_voxel_slice_key(std::string_view key, bool control, bool shift,
                                                    bool alt) {
    if (control || alt)
        return false;
    if (key == "escape") {
        if (sliceDrag_) { finish_slice_pointer(true); return true; }
        cancel_voxel_slice();
        return true;
    }
    if (key == "enter" || key == "return") {
        (void)commit_voxel_slice();
        return true;
    }
    if (sliceDrag_) return true;
    if (key == "s") { sliceSnap_ = !sliceSnap_; return true; }
    if (key == "p" || key == "n") {
        const auto value = key == "p" ? voxelSlice_.point() : voxelSlice_.normal();
        begin_text_edit(key == "p" ? TextEditKind::SlicePoint : TextEditKind::SliceNormal, voxelSlice_.target(),
            std::to_string(value.x) + " " + std::to_string(value.y) + " " + std::to_string(value.z));
        return true;
    }
    auto point = voxelSlice_.point();
    auto normal = voxelSlice_.normal();
    auto output = voxelSlice_.output();
    if (key == "x")
        normal = {1, 0, 0};
    else if (key == "y")
        normal = {0, 1, 0};
    else if (key == "z")
        normal = {0, 0, 1};
    else if (key == "1")
        output = VoxelSliceOutput::Front;
    else if (key == "2")
        output = VoxelSliceOutput::Back;
    else if (key == "3")
        output = VoxelSliceOutput::Separate;
    else if (key == "left" || key == "down" || key == "right" || key == "up") {
        const auto* o = workspace_.document().find_object(voxelSlice_.target());
        if (!o) {
            cancel_voxel_slice();
            return true;
        }
        float delta = o->voxelSizeMeters * (shift ? 0.1F : 1.0F);
        if (key == "left" || key == "down")
            delta = -delta;
        point = add(point, multiply(normal, delta));
    } else
        return false;
    (void)configure_voxel_slice(point, normal, output);
    return true;
}

bool NativeEditorController::voxel_authoring_active() const noexcept {
    return voxelAuthoring_.kind != VoxelAuthoringState::Kind::Inactive;
}
bool NativeEditorController::voxel_authoring_preview_ready() const noexcept {
    return voxel_authoring_active() && voxelAuthoring_.ready && voxelAuthoring_.preview.has_value();
}
bool NativeEditorController::voxel_preview_replaces_cell(EditorObjectId object, Int3 cell) const noexcept {
    return voxel_authoring_preview_ready() && voxelAuthoring_.kind == VoxelAuthoringState::Kind::Region &&
           voxelAuthoring_.object == object && voxelSelection_.contains(cell);
}
void NativeEditorController::cancel_voxel_authoring_preview() {
    ++voxelAuthoringGeneration_;
    voxelAuthoring_ = {};
}

bool NativeEditorController::begin_voxel_region_preview(VoxelRegionEdit edit, VoxelRegionSettings settings) {
    prune_voxel_selection();
    if (const auto reason = voxel_region_disabled_reason(); !reason.empty()) { set_status(reason,true); return false; }
    const auto* source = workspace_.document().find_object(voxelSelectionObject_);
    if (source->voxels->occupied_voxel_count() > kMaxConversionWorkingCells) {
        set_status("Region preview supports objects up to 2000000 cells; split the object first",true); return false;
    }
    cancel_mesh_authoring_drag(); meshAuthoring_.reset();
    close_previews_for_voxel_edit("voxel region preview");
    if (edit == VoxelRegionEdit::RotateQuarterTurn && !settings.rotationPivot &&
        voxelRegionRotationObject_ == source->id && voxelRegionRotationSerial_ == voxelSelectionSerial_)
        settings.rotationPivot = voxelRegionRotationPivot_;
    voxelAuthoring_.kind = VoxelAuthoringState::Kind::Region;
    voxelAuthoring_.object = source->id;
    voxelAuthoring_.edit = edit; voxelAuthoring_.settings = settings;
    if (settings.rotationPivot) { voxelAuthoring_.pivotMode = 2; voxelAuthoring_.customPivot = *settings.rotationPivot; }
    return update_voxel_authoring_preview();
}

bool NativeEditorController::begin_geometry_conversion(bool toVoxels,bool subdivisionOutput) {
    if (longJob_ || workspace_.mode() != EditorMode::Edit || playSession_.active() || workspace_.selection_count() != 1) {
        set_status(workspace_.selection_count() != 1 ? "Select exactly one object to convert"
                                                     : "Stop Play and finish background work before converting",true);
        return false;
    }
    const auto* source = workspace_.document().find_object(*workspace_.selected_object());
    if (!source || !source->flags.visible || source->flags.locked || is_isolated_out(source->id) ||
        (toVoxels ? !source->polygon : !source->voxels || source->polygon || source->text3d || source->gaborVolume)) {
        set_status("Select one visible unlocked object of the conversion's source type",true); return false;
    }
    if ((source->polygon && (source->polygon->vertices.size() > 1'000'000 || source->polygon->indices.size() > 1'500'000)) ||
        (!toVoxels && source->voxels->occupied_voxel_count() > 250'000)) {
        set_status("Split this object before converting: conversion size limit exceeded",true); return false;
    }
    if (source->polygon) {
        std::size_t bytes = 0;
        for (const auto& image : source->polygon->images) {
            if (image.rgba8.size() > 64U*1024U*1024U-bytes) {
                set_status("Conversion texture data exceeds 64 MiB; reduce source textures first",true); return false;
            }
            bytes += image.rgba8.size();
        }
    }
    cancel_mesh_authoring_drag(); meshAuthoring_.reset();
    close_previews_for_voxel_edit("geometry conversion");
    voxelAuthoring_.kind = toVoxels ? VoxelAuthoringState::Kind::PolygonToVoxel : VoxelAuthoringState::Kind::VoxelToPolygon;
    voxelAuthoring_.object = source->id;
    voxelAuthoring_.output = workspace_.document().allocate_object_id();
    voxelAuthoring_.subdivisionOutput = subdivisionOutput;
    voxelAuthoring_.conversion.voxelSizeMeters = std::clamp(source->voxelSizeMeters,0.005F,10.0F);
    return update_voxel_authoring_preview();
}

std::string NativeEditorController::voxel_authoring_field_value(int field) const {
    const auto& s = voxelAuthoring_;
    if (field==0 && s.kind==VoxelAuthoringState::Kind::VoxelToPolygon && s.subdivisionOutput)
        return std::to_string(s.subdivisionLevel);
    if (field != 0) return std::to_string(field == 1 ? s.customPivot.x : field == 2 ? s.customPivot.y : s.customPivot.z);
    if (s.kind == VoxelAuthoringState::Kind::PolygonToVoxel) return std::to_string(s.conversion.voxelSizeMeters);
    return std::to_string(s.edit == VoxelRegionEdit::RotateQuarterTurn ? s.settings.amount*90 : s.settings.amount);
}
bool NativeEditorController::commit_voxel_authoring_field() {
    auto& s = voxelAuthoring_;
    if (!s.fieldEditing) return true;
    if (s.field == 0 && s.kind == VoxelAuthoringState::Kind::PolygonToVoxel) {
        float value{};
        const auto [end,error] = std::from_chars(s.typed.data(),s.typed.data()+s.typed.size(),value);
        if (error != std::errc{} || end != s.typed.data()+s.typed.size() || !std::isfinite(value) || value < 0.005F || value > 10) {
            s.error = "Voxel size must be 0.005..10"; set_status(s.error,true); return false;
        }
        s.conversion.voxelSizeMeters = value;
    } else {
        std::int32_t value{};
        const auto [end,error] = std::from_chars(s.typed.data(),s.typed.data()+s.typed.size(),value);
        if (error != std::errc{} || end != s.typed.data()+s.typed.size()) {
            s.error = "Enter a whole number"; set_status(s.error,true); return false;
        }
        if (s.field==0 && s.kind==VoxelAuthoringState::Kind::VoxelToPolygon && s.subdivisionOutput) {
            if (value<0 || value>4) {s.error="Subdivision level must be 0..4";set_status(s.error,true);return false;}
            s.subdivisionLevel=static_cast<std::uint32_t>(value);
        } else if (s.field != 0) {
            if (value < kInt3Min.x+kBrickDim || value > kInt3Max.x-kBrickDim) { s.error = "Pivot exceeds the grid"; return false; }
            if (s.field == 1) s.customPivot.x = value;
            else if (s.field == 2) s.customPivot.y = value; else s.customPivot.z = value;
            s.pivotMode = 2;
        } else {
            const bool valid = s.edit == VoxelRegionEdit::Extrude ? value != 0 && value >= -32 && value <= 32 :
                s.edit == VoxelRegionEdit::RotateQuarterTurn ? value == -90 || value == 90 : value >= 2 && value <= 8;
            if (!valid) { s.error = "Distance +/-1..32, turn +/-90, scale 2..8"; set_status(s.error,true); return false; }
            s.settings.amount = s.edit == VoxelRegionEdit::RotateQuarterTurn ? value/90 : value;
        }
    }
    s.fieldEditing = false; s.typed.clear(); s.error.clear(); s.ready = false; s.preview.reset();
    return true;
}

bool NativeEditorController::update_voxel_authoring_preview() {
    if (!voxel_authoring_active() || longJob_ || workspace_.mode() != EditorMode::Edit || playSession_.active() ||
        !commit_voxel_authoring_field()) return false;
    auto& s = voxelAuthoring_;
    const auto* source = workspace_.document().find_object(s.object);
    if (!source || source->flags.locked || !source->flags.visible || is_isolated_out(s.object)) {
        s.error = "Source is missing, locked or hidden"; s.ready = false; s.preview.reset(); return false;
    }
    if (s.kind == VoxelAuthoringState::Kind::Region &&
        (selectionDomain_ != SelectionDomain::Voxels || voxelSelectionObject_ != s.object || voxelSelection_.empty())) {
        s.error = "Region selection changed; reopen the tool"; return false;
    }
    if (s.kind == VoxelAuthoringState::Kind::PolygonToVoxel) {
        std::array<bool,256> reserved{};
        for (const auto& entry : materials_.entries()) reserved[entry.id] = true;
        for (const auto& [id,object] : workspace_.document().objects()) {
            (void)id;
            for (const auto& [material,definition] : object.voxelMaterials) { (void)definition; reserved[material] = true; }
            if (!object.voxels) continue;
            for (const auto& [key,brick] : object.voxels->bricks()) {
                (void)key;
                brick.occupancy().for_each_set([&](std::uint16_t at) { reserved[brick.material(at)] = true; });
            }
        }
        s.paletteSlots.clear();
        for (unsigned id = 1; id < 256; ++id) if (!reserved[id]) s.paletteSlots.push_back(static_cast<MaterialId>(id));
    }
    if (s.kind == VoxelAuthoringState::Kind::Region && s.edit == VoxelRegionEdit::RotateQuarterTurn) {
        if (s.pivotMode == 0) {
            Int3 minimum = *voxelSelection_.begin(), maximum = minimum;
            for (auto cell : voxelSelection_) { minimum = min_components(minimum,cell); maximum = max_components(maximum,cell); }
            s.customPivot = {static_cast<std::int32_t>(std::int64_t(minimum.x)+(std::int64_t(maximum.x)-minimum.x)/2),
                static_cast<std::int32_t>(std::int64_t(minimum.y)+(std::int64_t(maximum.y)-minimum.y)/2),
                static_cast<std::int32_t>(std::int64_t(minimum.z)+(std::int64_t(maximum.z)-minimum.z)/2)};
        }
        s.settings.rotationPivot = s.pivotMode == 1 ? Int3{} : s.customPivot;
    }
    s.ready = false; s.preview.reset(); s.error.clear(); s.note = "Computing preview";
    s.documentRevision = scene_change_fingerprint(workspace_.document(),workspace_.commands());
    s.selectionSerial = voxelSelectionSerial_;
    const auto generation = ++voxelAuthoringGeneration_;
    const auto kind = s.kind; const auto operation = s.edit; const auto settings = s.settings;
    const auto conversion = s.conversion; const auto output = s.output; const auto slots = s.paletteSlots;
    const bool subdivisionOutput = s.subdivisionOutput;
    const auto subdivisionLevel = s.subdivisionLevel;
    auto snapshot = std::make_shared<EditorObject>(clone_editor_object(*source));
    const auto selection = voxelSelection_;
    const auto library = materials_;
    auto prepared = std::make_shared<GeometryConversionBuild>();
    return start_long_job(LongJobKind::ImportModel,"Voxel/conversion preview",longJobThreshold_+1,
        [snapshot,selection,kind,operation,settings,conversion,output,slots,library,prepared,subdivisionOutput,subdivisionLevel](LongJob& job) {
            if (job.cancelled()) return;
            job.report(0.1F,"Building grid geometry");
            try {
                if (kind == VoxelAuthoringState::Kind::Region) {
                    auto built = build_voxel_region_command(*snapshot,selection,operation,settings);
                    if (!built.command) prepared->error = built.error;
                    else {
                        EditorDocument preview("Voxel region preview");
                        preview.add_object(std::move(*snapshot));
                        const auto result = built.command->execute(preview);
                        if (!result.success) prepared->error = result.message;
                        else {
                            prepared->object = clone_editor_object(preview.objects().begin()->second);
                            prepared->selectedCells = std::move(built.selection);
                        }
                        prepared->note = "Grid result; original scene is unchanged until Apply.";
                    }
                } else if (kind == VoxelAuthoringState::Kind::PolygonToVoxel)
                    *prepared = polygon_to_voxels(*snapshot,output,conversion,slots);
                else {
                    *prepared = voxels_to_polygon(*snapshot,output,library);
                    if (subdivisionOutput && prepared->object) {
                        auto& object=*prepared->object;object.subdivision=SubdivisionSettings{subdivisionLevel,false};
                        auto evaluated=evaluate_subdivision(*object.polygon,*object.subdivision);
                        if (!evaluated.mesh) {prepared->error=evaluated.error;prepared->object.reset();}
                        else {object.subdivisionCache=std::move(evaluated);object.subdivisionCacheHash=object.polygon->contentHash;
                            object.subdivisionCacheSettings=*object.subdivision;object.name=snapshot->name+" (subdivision)";
                            prepared->note="Editable subdivision cage; source voxels retained.";}
                    }
                }
            } catch (const std::exception& exception) { prepared->error = exception.what(); }
            if (job.cancelled()) return;
            job.report(1.0F,"Preview ready");
        },
        [this,prepared,generation](LongJob&) {
            if (generation != voxelAuthoringGeneration_ || !voxel_authoring_active()) return false;
            auto& state = voxelAuthoring_;
            state.note = prepared->note; state.error = prepared->error;
            if (!prepared->object) { state.ready = false; refresh_menu_state(); set_status(state.error,true,6.0F); return false; }
            state.previewCells = std::move(prepared->selectedCells);
            state.preview.emplace("Voxel/conversion preview");
            state.preview->add_object(std::move(*prepared->object));
            state.ready = true;
            refresh_menu_state();
            set_status("Preview ready: adjust settings and Update, then Apply or Cancel; Enter applies, Esc cancels",false,7.0F);
            return true;
        });
}

bool NativeEditorController::apply_voxel_authoring_preview() {
    if (!voxel_authoring_active() || longJob_ || workspace_.mode() != EditorMode::Edit || playSession_.active()) return false;
    if (voxelAuthoring_.fieldEditing) {
        if (!commit_voxel_authoring_field()) return false;
        return update_voxel_authoring_preview(); // Changed input must be previewed before it can apply.
    }
    if (!voxel_authoring_preview_ready()) {
        // Never report success for an Apply that applied nothing (a failed or pending preview).
        set_status(voxelAuthoring_.error.empty() ? "Update the preview before applying" : voxelAuthoring_.error,true);
        return false;
    }
    auto& s = voxelAuthoring_;
    if (s.documentRevision != scene_change_fingerprint(workspace_.document(),workspace_.commands()) ||
        (s.kind == VoxelAuthoringState::Kind::Region && (s.selectionSerial != voxelSelectionSerial_ ||
         selectionDomain_ != SelectionDomain::Voxels || voxelSelectionObject_ != s.object))) {
        (void)update_voxel_authoring_preview();
        set_status("Scene or region changed: inspect the updated preview before applying",true);
        return false;
    }
    const auto* source = workspace_.document().find_object(s.object);
    if (!source || source->flags.locked || !source->flags.visible || is_isolated_out(s.object)) return false;
    if (s.kind == VoxelAuthoringState::Kind::Region) {
        const auto operation = s.edit; const auto settings = s.settings;
        return edit_selected_voxel_region(operation,settings);
    }
    const auto id = s.output;
    const auto* previewObject = s.preview->find_object(id);
    if (!previewObject) return false;
    for (const auto& [material,definition] : previewObject->voxelMaterials) {
        (void)definition;
        if (materials_.find(material)) {
            (void)update_voxel_authoring_preview();
            set_status("Paint palette changed; inspect the updated conversion preview",true); return false;
        }
    }
    auto object = clone_editor_object(*previewObject);
    const auto before = capture_selection_snapshot();
    const auto result = workspace_.commands().execute(workspace_.document(),
        std::make_unique<AddObjectCommand>(std::move(object),"Convert geometry (keep source)"));
    if (!result.success) { set_status(result.message,true); return false; }
    cancel_voxel_authoring_preview(); reset_voxel_selection_state();
    selectionDomain_ = SelectionDomain::Objects; polygonSelection_ = {};
    if (!viewportSettings_.isolatedObjects.empty()) viewportSettings_.isolatedObjects.insert(id);
    workspace_.select_object(id);
    workspace_.commands().set_last_selection_transition(before,capture_selection_snapshot());
    recompute_layout(); refresh_menu_state();
    set_status("Created converted object; source retained. Undo removes the result and restores selection.",false,6.0F);
    return true;
}

NativeEditorController::VoxelAuthoringLayout NativeEditorController::voxel_authoring_layout(UiRect viewport) const {
    VoxelAuthoringLayout result;
    result.panel = {viewport.x+8,viewport.y+8,std::max(1,std::min(370,viewport.width-16)),std::max(1,std::min(274,viewport.height-16))};
    const auto p = result.panel; const int width = std::max(1,(p.width-24)/3);
    result.amount = {p.x+12,p.y+56,p.width-24,24};
    for (int i = 0; i < 3; ++i) {
        result.axes[i] = {p.x+12+i*width,p.y+28,width-3,22};
        result.pivots[i] = {p.x+12+i*width,p.y+86,width-3,22};
        result.coordinates[i] = {p.x+12+i*width,p.y+114,width-3,22};
    }
    result.refresh = {p.x+12,p.y+144,p.width-24,24};
    result.apply = {p.x+12,p.y+174,(p.width-30)/2,24};
    result.cancel = {p.x+18+(p.width-30)/2,p.y+174,(p.width-30)/2,24};
    return result;
}

bool NativeEditorController::voxel_authoring_pointer_down(PointerButton button,int x,int y,std::uint32_t modifiers) {
    if (!voxel_authoring_active() || button != PointerButton::Primary || (modifiers&4U)) return false;
    const auto panel = voxel_authoring_layout(layout_.viewport);
    auto& s = voxelAuthoring_;
    // Preview owns authoring input; menus can still expose view commands, and camera
    // navigation uses non-primary or Alt gestures. Do not edit the inspector behind it.
    if (!panel.panel.contains(x,y)) return !layout_.menuBar.contains(x,y) && !open_menu();
    if (panel.cancel.contains(x,y)) { cancel_voxel_authoring_preview(); refresh_menu_state(); set_status("Preview cancelled; scene unchanged"); return true; }
    if (panel.apply.contains(x,y)) { (void)apply_voxel_authoring_preview(); return true; }
    if (panel.refresh.contains(x,y)) { (void)update_voxel_authoring_preview(); return true; }
    const auto beginField = [&](int field) {
        if (!commit_voxel_authoring_field()) return;
        s.field = field; s.typed = voxel_authoring_field_value(field); s.fieldEditing = true; s.replaceTyped = true;
    };
    if (panel.amount.contains(x,y) && (s.kind != VoxelAuthoringState::Kind::VoxelToPolygon || s.subdivisionOutput)) { beginField(0); return true; }
    for (int i = 0; i < 3; ++i) {
        if (panel.axes[i].contains(x,y)) {
            if (!commit_voxel_authoring_field()) return true;
            if (s.kind == VoxelAuthoringState::Kind::PolygonToVoxel) s.conversion.mode = static_cast<VoxelizationMode>(i);
            else if (s.kind == VoxelAuthoringState::Kind::Region) s.settings.axis = i;
            (void)update_voxel_authoring_preview(); return true;
        }
        if (s.kind == VoxelAuthoringState::Kind::Region && s.edit == VoxelRegionEdit::RotateQuarterTurn) {
            if (panel.pivots[i].contains(x,y)) {
                if (!commit_voxel_authoring_field()) return true;
                s.pivotMode = i; (void)update_voxel_authoring_preview(); return true;
            }
            if (panel.coordinates[i].contains(x,y)) { beginField(i+1); return true; }
        }
    }
    return true;
}

bool NativeEditorController::voxel_authoring_key(std::string_view key,bool control,bool shift,bool alt) {
    (void)shift;
    if (!voxel_authoring_active() || control || alt) return false;
    auto& s = voxelAuthoring_;
    if (key == "escape") { cancel_voxel_authoring_preview(); refresh_menu_state(); set_status("Preview cancelled; scene unchanged"); return true; }
    if (key == "return" || key == "enter") { (void)apply_voxel_authoring_preview(); return true; }
    if (key == "tab" && (s.kind != VoxelAuthoringState::Kind::VoxelToPolygon || s.subdivisionOutput)) {
        if (!commit_voxel_authoring_field()) return true;
        s.field = s.kind == VoxelAuthoringState::Kind::Region && s.edit == VoxelRegionEdit::RotateQuarterTurn ? (s.field+1)%4 : 0;
        s.typed = voxel_authoring_field_value(s.field); s.fieldEditing = true; s.replaceTyped = true; return true;
    }
    if (s.kind == VoxelAuthoringState::Kind::Region && (key == "x" || key == "y" || key == "z")) {
        if (!commit_voxel_authoring_field()) return true;
        s.settings.axis = key == "x" ? 0 : key == "y" ? 1 : 2;
        (void)update_voxel_authoring_preview(); return true;
    }
    const char c = key == "period" ? '.' : key == "minus" ? '-' : key.size() == 1 ? key[0] : '\0';
    if (key == "backspace" || (c >= '0' && c <= '9') || c == '.' || c == '-') {
        if (s.kind == VoxelAuthoringState::Kind::VoxelToPolygon && !s.subdivisionOutput) return true;
        if (!s.fieldEditing) { s.field = 0; s.fieldEditing = true; s.typed.clear(); s.replaceTyped = false; }
        if (s.replaceTyped) { s.typed.clear(); s.replaceTyped = false; }
        if (key == "backspace") { if (!s.typed.empty()) s.typed.pop_back(); }
        else if (s.typed.size() < 24) s.typed.push_back(c);
        return true;
    }
    return false;
}

void NativeEditorController::draw_voxel_authoring(const IEditorCanvas& canvas,UiRect viewport) const {
    const auto& s = voxelAuthoring_;
    if (!voxel_authoring_active()) return;
    const EditorColor ink = 0xe0e9f5, accent = 0x65d7ff, button = 0x293746;
    if (s.preview && s.ready) {
        EditorViewportSettings settings; settings.showGrid = false; settings.maximumDrawVoxels = 20000;
        if (s.kind == VoxelAuthoringState::Kind::Region) {
            const auto* object = s.preview->find_object(s.object);
            std::size_t drawn = 0;
            if (object) for (const auto cell : s.previewCells) {
                if (++drawn > 20000) break;
                const auto centre = voxel_to_world(object->transform,multiply(object->voxelScale,object->voxelSizeMeters),
                    {float(cell.x)+0.5F,float(cell.y)+0.5F,float(cell.z)+0.5F});
                const auto point = project_world_to_screen(camera_,viewport,centre);
                if (!point.visible) continue;
                float extentX = 1,extentY = 1;
                for (int corner = 0; corner < 8; ++corner) {
                    const auto world = voxel_to_world(object->transform,multiply(object->voxelScale,object->voxelSizeMeters),
                        {float(cell.x)+float(corner&1),float(cell.y)+float((corner>>1)&1),float(cell.z)+float((corner>>2)&1)});
                    const auto screen = project_world_to_screen(camera_,viewport,world);
                    if (screen.depth > camera_.nearPlane) {
                        extentX = std::max(extentX,std::abs(screen.x-point.x));
                        extentY = std::max(extentY,std::abs(screen.y-point.y));
                    }
                }
                const int rx = int(std::min(extentX,float(viewport.width))),ry = int(std::min(extentY,float(viewport.height)));
                canvas.outline({int(point.x)-rx,int(point.y)-ry,rx*2+1,ry*2+1},accent);
            }
        } else {
            const auto voxels = build_voxel_draw_list(*s.preview,materials_,camera_,viewport,settings,std::set<EditorObjectId>{});
            for (const auto& voxel : voxels) {
                const int rx = std::max(1,int(voxel.pixelExtentX > 0 ? voxel.pixelExtentX : voxel.pixelRadius));
                const int ry = std::max(1,int(voxel.pixelExtentY > 0 ? voxel.pixelExtentY : voxel.pixelRadius));
                canvas.outline({int(voxel.screenX)-rx,int(voxel.screenY)-ry,rx*2+1,ry*2+1},accent);
            }
        }
        for (const auto& triangle : build_editor_polygon_draw_list(*s.preview,camera_,viewport,{}, {},20000))
            for (unsigned i = 0; i < 3; ++i) {
                const auto a = triangle.points[i], b = triangle.points[(i+1)%3];
                canvas.line(int(a.x),int(a.y),int(b.x),int(b.y),accent,1);
            }
    }
    const auto ui = voxel_authoring_layout(viewport);
    RectClipCanvas painter(canvas,ui.panel);
    painter.fill(ui.panel,0x17202b); painter.outline(ui.panel,accent);
    const auto label = [&](UiRect rect,std::string text,bool selected = false) {
        painter.fill(rect,selected ? 0x456782 : button);
        painter.text(rect.x+5,rect.y+16,elide_text_to_width(painter,text,rect.width-10),ink);
    };
    const bool region = s.kind == VoxelAuthoringState::Kind::Region;
    const std::string title = region ? s.edit == VoxelRegionEdit::Extrude ? "EXTRUDE VOXEL REGION" :
        s.edit == VoxelRegionEdit::RotateQuarterTurn ? "ROTATE VOXEL REGION" : "SCALE VOXEL REGION" :
        s.kind == VoxelAuthoringState::Kind::PolygonToVoxel ? "MESH TO VOXELS" :
        s.subdivisionOutput ? "VOXELS TO SUBDIVISION" : "VOXELS TO POLYGON";
    painter.text(ui.panel.x+12,ui.panel.y+20,title,accent);
    for (int i = 0; i < 3; ++i)
        if (s.kind == VoxelAuthoringState::Kind::PolygonToVoxel)
            label(ui.axes[i],i == 0 ? "Solid" : i == 1 ? "Shell" : "Surface",int(s.conversion.mode) == i);
        else if (region && s.edit != VoxelRegionEdit::ScaleInteger) label(ui.axes[i],i == 0 ? "Grid X" : i == 1 ? "Grid Y" : "Grid Z",s.settings.axis == i);
    if ((s.kind != VoxelAuthoringState::Kind::VoxelToPolygon || s.subdivisionOutput)) {
        const std::string prefix = s.kind==VoxelAuthoringState::Kind::VoxelToPolygon && s.subdivisionOutput ? "Subdivision level (0..4): " :
            s.kind == VoxelAuthoringState::Kind::PolygonToVoxel ? "Local cell metres: " :
            s.edit == VoxelRegionEdit::Extrude ? "Signed cells: " : s.edit == VoxelRegionEdit::RotateQuarterTurn ? "Degrees (+/-90): " : "Integer factor (2..8): ";
        label(ui.amount,prefix+(s.fieldEditing && s.field == 0 ? s.typed : voxel_authoring_field_value(0)),s.fieldEditing && s.field == 0);
    }
    if (region && s.edit == VoxelRegionEdit::RotateQuarterTurn) {
        for (int i = 0; i < 3; ++i) {
            label(ui.pivots[i],i == 0 ? "Bounds" : i == 1 ? "Cell 0" : "Custom",s.pivotMode == i);
            label(ui.coordinates[i],std::string(i == 0 ? "X: " : i == 1 ? "Y: " : "Z: ")+
                (s.fieldEditing && s.field == i+1 ? s.typed : voxel_authoring_field_value(i+1)),s.fieldEditing && s.field == i+1);
        }
    }
    label(ui.refresh,"Update Preview");
    label(ui.apply,s.ready ? "Apply (Enter)" : "Preview first",s.ready);
    label(ui.cancel,"Cancel (Esc)");
    painter.text(ui.panel.x+12,ui.panel.y+218,region ? "Object grid | source unchanged until Apply" : "New sibling | source retained | Undo supported",ink);
    painter.text(ui.panel.x+12,ui.panel.y+240,elide_text_to_width(painter,s.error.empty() ? s.note : s.error,ui.panel.width-24),
        s.error.empty() ? ink : 0xffae83);
    painter.text(ui.panel.x+12,ui.panel.y+260,"Display capped at 20k cells / triangles",ink);
}

} // namespace dve::editor
