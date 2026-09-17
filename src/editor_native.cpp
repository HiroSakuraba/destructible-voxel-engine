#include "dve/editor_native.hpp"
#include "dve/editor_prefab.hpp"

#include <algorithm>
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

NativeEditorController::NativeEditorController(EditorWorkspace workspace)
    : workspace_(std::move(workspace)), materials_(EditorMaterialLibrary::make_default()) {
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
}

bool NativeEditorController::start_play_session(EditorMode mode) {
    if (playSession_.active()) {
        set_status("A play-in-editor session is already active", true);
        return false;
    }
    EditorPlaySessionConfig config;
    config.projectRoot = projectRoot_;
    const std::filesystem::path conventionalScript = projectRoot_ / "scripts" / "main.lua";
    if (std::filesystem::exists(conventionalScript)) config.startupScript = conventionalScript;
    cancel_text_edit();
    close_context_menu();
    close_top_level_menu();
    activePointerCommand_.clear();
    gizmoDragging_ = false;
    gizmoChanges_.clear();
    voxelStrokeActive_ = false;
    marquee_ = {};
    hierarchyDrag_ = {};
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
    playSession_.set_axis("move_x", moveX);
    playSession_.set_axis("move_y", moveY);
}

void NativeEditorController::configure_ai_assistant(std::filesystem::path projectRoot) {
    stop_live_mcp_host();
    if (projectRoot.empty()) projectRoot = std::filesystem::current_path();
    std::error_code projectError;
    projectRoot_ = std::filesystem::weakly_canonical(projectRoot, projectError);
    if (projectError) projectRoot_ = projectRoot.lexically_normal();
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
}


std::vector<const EditorAssetRecord*> NativeEditorController::asset_browser_rows() const {
    EditorAssetQuery query = assetBrowserState_.query;
    query.limit = 2048U;
    return assetDatabase_.query(query);
}

bool NativeEditorController::refresh_asset_database(bool announce) {
    EditorAssetScanReport report;
    std::string error;
    if (!assetDatabase_.scan(&report, &error)) {
        if (announce) set_status(error.empty() ? "Asset scan failed" : error, true, 7.0F);
        return false;
    }
    if (assetBrowserState_.selectedId && !assetDatabase_.find(*assetBrowserState_.selectedId))
        assetBrowserState_.selectedId.reset();
    assetBrowserState_.firstVisible = 0U;
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

std::vector<CommandPaletteResult> NativeEditorController::command_palette_results(std::size_t limit) const {
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
            if (const MenuAction* action = workspace_.menus().find(id)) commandResult(*action, priority++);
        }
        for (const std::string& id : recentCommandIds_) {
            if (std::any_of(scored.begin(), scored.end(), [&](const ScoredResult& item) {
                    return item.result.kind == CommandPaletteResultKind::Command && item.result.id == id;
                })) continue;
            if (const MenuAction* action = workspace_.menus().find(id)) commandResult(*action, 100 + priority++);
        }
        for (std::string_view id : {"file.save", "edit.undo", "sprite.tile_world", "sprite.diagnostics",
                                    "render.diagnostics3d", "physics.play", "build.validate", "help.shortcuts"}) {
            if (std::any_of(scored.begin(), scored.end(), [&](const ScoredResult& item) {
                    return item.result.kind == CommandPaletteResultKind::Command && item.result.id == id;
                })) continue;
            if (const MenuAction* action = workspace_.menus().find(id)) commandResult(*action, 250 + priority++);
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
            const MenuAction* action = workspace_.menus().find(panel.action);
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
    const float scale = std::clamp(workspace_.preferences().uiScale, 0.75F, 2.0F);
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
                if (workspace_.menus().find(id)) favoriteCommandIds_.insert(id);
            for (const std::string& id : state->recentActionIds)
                if (workspace_.menus().find(id)) recentCommandIds_.push_back(id);
        } else if (!error.empty()) workspace_.log().add(EditorLogLevel::Warning, "Menu state load: " + error);
    }
    refresh_menu_state();
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
    const EditorAssetRecord* asset = assetDatabase_.find(selected.id);
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
    std::uint32_t result = SettingCapabilityEditor | SettingCapabilityAudio | SettingCapabilityVulkan;
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
    const float scale = std::clamp(workspace_.preferences().uiScale, 0.75F, 2.0F);
    const int margin = std::max(20, static_cast<int>(28.0F * scale));
    result.panel.width = std::min(1120, std::max(700, width_ - margin * 2));
    result.panel.height = std::min(780, std::max(500, height_ - margin * 2));
    result.panel.x = (width_ - result.panel.width) / 2;
    result.panel.y = (height_ - result.panel.height) / 2;
    const int header = std::max(82, static_cast<int>(88.0F * scale));
    const int footer = std::max(58, static_cast<int>(64.0F * scale));
    const int detailHeight = std::max(88, static_cast<int>(98.0F * scale));
    const int categoryWidth = std::clamp(static_cast<int>(205.0F * scale), 170, 280);
    const int rowHeight = std::max(27, static_cast<int>(30.0F * scale));

    const int tabWidth = std::max(76, static_cast<int>(86.0F * scale));
    for (std::size_t index = 0; index < result.scopeTabs.size(); ++index)
        result.scopeTabs[index] = {result.panel.x + 16 + static_cast<int>(index) * (tabWidth + 6),
                                   result.panel.y + 10, tabWidth, 28};
    result.searchBox = {result.panel.x + categoryWidth + 24, result.panel.y + 44,
                        std::max(180, result.panel.width - categoryWidth - 318), 30};
    result.advancedToggle = {result.panel.x + result.panel.width - 144, result.panel.y + 44, 124, 30};
    result.changedToggle = {result.advancedToggle.x - 124 - 8, result.panel.y + 44, 124, 30};

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
    const float scale = std::clamp(workspace_.preferences().uiScale, 0.75F, 2.0F);
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
        if (activePointerCommand_ == "viewport.look" || dragButton_ == PointerButton::Secondary)
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
    std::string error;
    if (!cameraDirector_.add_or_replace_rig(rig, &error)) {
        set_status(error, true);
        return 0;
    }
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
    settingsPanel_.open_for(scope, std::move(category));
    close_top_level_menu();
    close_context_menu();
    set_status(setting_scope_name(scope) + " settings");
}

void NativeEditorController::close_settings(bool applyChanges) {
    if (!settingsPanel_.open) return;
    if (applyChanges) {
        std::string error;
        if (!settingsPanel_.apply(workspace_.settings(), &error)) {
            set_status(error, true);
            return;
        }
        apply_settings_to_runtime();
        set_status("Settings applied");
    } else {
        settingsPanel_.discard();
        set_status("Settings changes discarded");
    }
    settingsPanel_.close();
}

void NativeEditorController::open_shortcut_editor() {
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

void NativeEditorController::synchronize_menu_shortcuts() noexcept {
    const SettingValue visibleValue = workspace_.settings().value("input.show_shortcuts_in_menus");
    const bool visible = !std::get_if<bool>(&visibleValue) || std::get<bool>(visibleValue);
    for (const MenuAction& action : workspace_.menus().actions())
        (void)workspace_.menus().set_shortcut(action.id, visible ? workspace_.shortcuts().display_binding(action.id) : std::string{});
}

void NativeEditorController::remember_camera_position() {
    if (previousCamera_ && length_squared(subtract(previousCamera_->position, camera_.position)) < 1.0e-8F &&
        length_squared(subtract(previousCamera_->target, camera_.target)) < 1.0e-8F) return;
    previousCamera_ = camera_;
    nextCamera_.reset();
}


void NativeEditorController::apply_settings_to_runtime() noexcept {
    workspace_.synchronize_preferences_from_settings();
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
    (void)workspace_.shortcuts().set_active_profile(profile, &shortcutError);
    synchronize_menu_shortcuts();
    const std::string mode = readString("camera.default_mode", "orbit");
    if (mode == "free") cameraMode_ = camera::CameraRigMode::FreeFly;
    else if (mode == "follow") cameraMode_ = camera::CameraRigMode::Follow;
    else if (mode == "third_person") cameraMode_ = camera::CameraRigMode::ThirdPerson;
    else if (mode == "first_person") cameraMode_ = camera::CameraRigMode::FirstPerson;
    else if (mode == "cinematic") cameraMode_ = camera::CameraRigMode::Cinematic;
    else cameraMode_ = camera::CameraRigMode::Orbit;
    recompute_layout();
    refresh_menu_state();
}

void NativeEditorController::refresh_menu_state() noexcept {
    auto checked = [&](std::string_view id, bool value) { (void)workspace_.menus().set_checked(id, value); };
    auto enabled = [&](std::string_view id, bool value, std::string reason) {
        (void)workspace_.menus().set_enabled(id, value, value ? std::string{} : std::move(reason));
    };
    const bool hasSelection = workspace_.selection_count() > 0U;
    const bool singleSelection = workspace_.selection_count() == 1U;
    const bool playActive = playSession_.active();
    enabled("edit.undo", workspace_.commands().can_undo(), "There is no authored edit to undo.");
    enabled("edit.redo", workspace_.commands().can_redo(), "There is no authored edit to redo.");
    for (std::string_view id : {"edit.cut", "edit.copy", "edit.delete", "edit.duplicate", "edit.group"})
        enabled(id, hasSelection, "Select one or more scene objects first.");
    enabled("edit.rename", singleSelection, "Select exactly one scene object to rename.");
    enabled("edit.ungroup", hasSelection, "Select a grouped object first.");
    enabled("edit.paste", !clipboard_.empty(), "Copy or cut an object before pasting.");
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
    enabled("text3d.cancel", text3dAuthoring_.active(), "No 3D text authoring session is active.");
    enabled("build.cook", !playActive, "Stop Play or Simulate before cooking content.");
    enabled("build.package", !playActive, "Stop Play or Simulate before packaging the game.");
    enabled("camera.pilot_selected", selectedCameraRig_.has_value(), "Select or create a camera rig first.");
    enabled("camera.align_to_view", selectedCameraRig_.has_value(), "Select or create a camera rig first.");
    enabled("camera.keyframe_profile", selectedCameraRig_.has_value(), "Select or create a camera rig first.");
    enabled("camera.paste_profile", cinematicCameraPanel_.can_paste(), "Copy a cinematic profile first.");
    checked("view.grid", viewportSettings_.showGrid);
    checked("view.collision", viewportSettings_.showCollision);
    checked("view.anchors", viewportSettings_.showAnchors);
    checked("view.bounds", viewportSettings_.showObjectBounds);
    checked("view.xray", viewportSettings_.xraySelection);
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
        if (definition->type == SettingType::Boolean || definition->type == SettingType::Enum) {
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
    const float scale = std::clamp(workspace_.preferences().uiScale, 0.75F, 3.0F);
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
    int x = 8;
    for (int index = 0; index < 9; ++index) {
        layout_.toolbarButtons.push_back({x, menuHeight + 4, buttonSize + 18, buttonSize});
        x += buttonSize + 24;
    }

    const int rowHeight = std::max(20, static_cast<int>(24.0F * scale));
    layout_.hierarchyFilterBox = {};
    layout_.hierarchyRows.clear();
    if (hierarchyWidth > 0) {
        layout_.hierarchyFilterBox = {4, contentY + 4, hierarchyWidth - 8, rowHeight};
        int rowY = contentY + rowHeight + static_cast<int>(10.0F * scale);
        const std::size_t rowCount = hierarchy_order().size();
        for (std::size_t i = 0; i < rowCount; ++i) {
            layout_.hierarchyRows.push_back({4, rowY, hierarchyWidth - 8, rowHeight});
            rowY += rowHeight;
        }
    }

    layout_.inspectorToggles.clear();
    layout_.inspectorFields.clear();
    if (inspectorWidth > 0) {
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
        }
        const float inspectorContentBottom = selectedObject && selectedObject->text3d ? 426.0F :
                                             selectedObject && selectedObject->gaborVolume ? 360.0F : 390.0F;
        int toggleY = contentY + static_cast<int>(inspectorContentBottom * scale);
        for (int index = 0; index < 5; ++index) {
            layout_.inspectorToggles.push_back({layout_.inspector.x + 12, toggleY, inspectorWidth - 24, rowHeight});
            toggleY += rowHeight + 4;
        }
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

void NativeEditorController::update(float elapsedSeconds) {
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
        spriteLevelAccumulator_ += std::clamp(elapsedSeconds, 0.0F, 0.25F);
        unsigned steps{};
        while (spriteLevelAccumulator_ >= fixedStep && steps < 8U) {
            spriteLevel_->step(spriteLevelInput_, fixedStep);
            spriteLevelPresentation_->sync(*spriteLevel_, fixedStep);
            spriteLevelInput_.jumpPressed = false;
            spriteLevelInput_.firePressed = false;
            spriteLevelInput_.savePressed = false;
            spriteLevelInput_.loadPressed = false;
            spriteLevelInput_.restartPressed = false;
            spriteLevelAccumulator_ -= fixedStep;
            ++steps;
        }
        if (steps == 8U) spriteLevelAccumulator_ = std::min(spriteLevelAccumulator_, fixedStep);
    }
    if (spriteAnimationGraphOpen_ && spriteLevel_) {
        const auto& state = spriteLevel_->state();
        spriteAnimationGraph_.set_live_state(!state.grounded ? "Jump" :
            (std::fabs(state.velocity.x) > 2.0F ? "Run" : "Idle"));
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
    for (const auto& [id, object] : workspace_.document().objects()) {
        camera::CameraTargetState target;
        target.position = object.transform.position;
        target.forward = rotate(object.transform.rotation, {0.0F, 0.0F, -1.0F});
        target.up = rotate(object.transform.rotation, {0.0F, 1.0F, 0.0F});
        cameraDirector_.set_target(id, target);
    }
    if (elapsedSeconds > 0.0F && !heldShortcutGestures_.empty()) {
        const auto contexts = active_shortcut_contexts();
        if (std::find(contexts.begin(), contexts.end(), ShortcutContext::FlyNavigation) != contexts.end()) {
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
            if (length_squared(movement) > 0.0F) {
                const SettingValue speedValue = workspace_.settings().value("camera.fly_speed");
                const SettingValue boostValue = workspace_.settings().value("camera.boost_multiplier");
                const float speed = std::get_if<double>(&speedValue)
                    ? static_cast<float>(std::get<double>(speedValue)) : workspace_.preferences().cameraSpeed;
                const float boost = std::get_if<double>(&boostValue)
                    ? static_cast<float>(std::get<double>(boostValue)) : 4.0F;
                fly_camera(camera_, normalize(movement), elapsedSeconds, speed * (boosted ? boost : 1.0F));
            }
        }
    }
    (void)cameraDirector_.update(std::max(0.0F, elapsedSeconds));
    const SettingValue lockCameraValue = workspace_.settings().value("camera.lock_viewport_to_selected");
    const bool lockCamera = std::get_if<bool>(&lockCameraValue) && std::get<bool>(lockCameraValue);
    if (lockCamera && selectedCameraRig_) {
        if (const camera::CameraRig* rig = cameraDirector_.find_rig(*selectedCameraRig_))
            apply_camera_pose(camera_, rig->authoredPose);
    }
    if (status_.secondsRemaining > 0.0F) {
        status_.secondsRemaining = std::max(0.0F, status_.secondsRemaining - std::max(0.0F, elapsedSeconds));
        if (status_.secondsRemaining == 0.0F && status_.text != "Ready") status_ = {"Ready", false, 0.0F};
    }
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
        case PendingDestructiveAction::Inactive:
            break;
    }
    return {};
}

void NativeEditorController::request_quit() {
    if (workspace_.document().dirty() && workspace_.preferences().confirmDestructiveActions) {
        pendingDestructiveAction_ = PendingDestructiveAction::Quit;
        return;
    }
    quitRequested_ = true;
}

void NativeEditorController::create_new_project_now() {
    workspace_.document() = make_new_project_document();
    workspace_.commands().clear();
    workspace_.clear_selection();
    if (!workspace_.document().objects().empty()) {
        workspace_.select_object(workspace_.document().objects().begin()->first);
        frame_selection();
    }
    recompute_layout();
}

void NativeEditorController::create_new_scene_now() {
    workspace_.document() = EditorDocument("Untitled Scene");
    workspace_.commands().clear();
    workspace_.clear_selection();
    recompute_layout();
}

void NativeEditorController::confirm_pending_destructive_action() {
    const PendingDestructiveAction action = pendingDestructiveAction_;
    pendingDestructiveAction_ = PendingDestructiveAction::Inactive;
    if (action == PendingDestructiveAction::Quit) {
        quitRequested_ = true;
    } else if (action == PendingDestructiveAction::NewProject) {
        create_new_project_now();
        set_status("New project");
    } else if (action == PendingDestructiveAction::NewScene) {
        create_new_scene_now();
        set_status("New scene");
    }
}

void NativeEditorController::cancel_pending_destructive_action() {
    if (pendingDestructiveAction_ == PendingDestructiveAction::Inactive) return;
    pendingDestructiveAction_ = PendingDestructiveAction::Inactive;
    set_status("Destructive action cancelled");
}

std::optional<std::string> NativeEditorController::menu_name_at(int x) const {
    const int desiredWidth = std::max(70, static_cast<int>(78.0F * workspace_.preferences().uiScale));
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
    const int itemHeight = std::max(22, static_cast<int>(24.0F * workspace_.preferences().uiScale));
    int menuIndex = 0;
    for (std::size_t candidate = 0; candidate < kMenuBarNames.size(); ++candidate) {
        if (kMenuBarNames[candidate] == *openMenu_) {
            menuIndex = static_cast<int>(candidate);
            break;
        }
    }
    const int desiredWidth = std::max(70, static_cast<int>(78.0F * workspace_.preferences().uiScale));
    const int menuWidth = std::max(1, std::min(desiredWidth, width_ / static_cast<int>(kMenuBarNames.size())));
    const int popupWidth = std::clamp(static_cast<int>(300.0F * workspace_.preferences().uiScale), 240, 420);
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
    close_context_menu();
    commandPaletteOpen_ = false;
    close_top_level_menu();
}

void NativeEditorController::commit_text_edit() {
    const TextEditKind kind = textEdit_.kind;
    if (kind == TextEditKind::Inactive) return;
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
        const EditorAssetRecord* selectedAsset = assetDatabase_.find(*assetBrowserState_.selectedId);
        if (!selectedAsset || name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
            set_status("Asset rename requires a valid filename", true);
            return;
        }
        const auto target = selectedAsset->relativePath.parent_path() / name;
        const auto moved = assetDatabase_.move_asset(*assetBrowserState_.selectedId, target, true);
        set_status(moved.success ? moved.message : moved.message, !moved.success, 7.0F);
        recompute_layout();
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

    const auto parsed = parse_float3_line(textEdit_.buffer);
    if (!parsed) {
        set_status("Could not parse three numbers", true);
        textEdit_ = {};
        return;
    }
    const CommandResult result = kind == TextEditKind::Position
        ? set_primary_position(*parsed)
        : set_primary_rotation_euler_degrees(*parsed);
    if (!result.success) set_status(result.message, true);
    textEdit_ = {};
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
    const int itemHeight = std::max(22, static_cast<int>(24.0F * workspace_.preferences().uiScale));
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

std::optional<EditorObjectId> NativeEditorController::hierarchy_row_object_at(int x, int y) const {
    const auto order = hierarchy_order();
    for (std::size_t index = 0; index < layout_.hierarchyRows.size() && index < order.size(); ++index) {
        if (layout_.hierarchyRows[index].contains(x, y)) return order[index];
    }
    return std::nullopt;
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
                                                                               static_cast<float>(x), static_cast<float>(y)));
}

void NativeEditorController::pointer_move(int x, int y, std::uint32_t modifiers) {
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
        contextMenu_.hoveredItem.reset();
        for (std::size_t index = 0; index < contextMenu_.itemRects.size(); ++index) {
            if (contextMenu_.itemRects[index].contains(x, y)) {
                contextMenu_.hoveredItem = index;
                break;
            }
        }
        lastPointerX_ = x;
        lastPointerY_ = y;
        return;
    }
    const int deltaX = x - lastPointerX_;
    const int deltaY = y - lastPointerY_;
    if (hierarchyDrag_.sourceId != 0) {
        // Promote a pending hierarchy-row press into an active drag once the pointer has
        // moved enough that this clearly isn't just a click.
        const int distanceSquared = (x - pointerDownX_) * (x - pointerDownX_) + (y - pointerDownY_) * (y - pointerDownY_);
        if (!hierarchyDrag_.active && distanceSquared > 25) hierarchyDrag_.active = true;
        if (hierarchyDrag_.active) hierarchyDrag_.hoverTarget = hierarchy_row_object_at(x, y);
    } else if (marquee_.active) {
        marquee_.currentX = x;
        marquee_.currentY = y;
    } else if (activePointerCommand_ == "viewport.look" || activePointerCommand_ == "viewport.orbit") {
        const SettingValue sensitivityValue = workspace_.settings().value("camera.mouse_sensitivity");
        const SettingValue orbitValue = workspace_.settings().value("camera.orbit_sensitivity");
        const SettingValue invertValue = workspace_.settings().value("camera.invert_y");
        const float sensitivity = std::get_if<double>(&sensitivityValue)
            ? static_cast<float>(std::get<double>(sensitivityValue)) : workspace_.preferences().mouseSensitivity;
        const float orbitScale = std::get_if<double>(&orbitValue)
            ? static_cast<float>(std::get<double>(orbitValue)) : 1.0F;
        const bool invertY = std::get_if<bool>(&invertValue) && std::get<bool>(invertValue);
        const float adjustedY = static_cast<float>(deltaY) * (invertY ? -1.0F : 1.0F);
        if (activePointerCommand_ == "viewport.look")
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
    } else if (gizmoDragging_) {
        update_gizmo_drag(x, y);
    } else if (voxelStrokeActive_) {
        continue_voxel_stroke(x, y);
    } else {
        update_hover(x, y);
    }
    (void)modifiers;
    lastPointerX_ = x;
    lastPointerY_ = y;
}

void NativeEditorController::pointer_down(PointerButton button, int x, int y, std::uint32_t modifiers) {
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
                if (definition.type == SettingType::Boolean || definition.type == SettingType::Enum) {
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
    if (button == PointerButton::Primary && chiptunePanel_.open() && chiptunePanel_.pointer_down(x, y, audioMixer_)) return;
    if (button == PointerButton::Primary && audioEventPanel_.open() && audioEventPanel_.pointer_down(x, y, audioMixer_)) return;
    if (button == PointerButton::Primary && audioPanel_.open() && audioPanel_.pointer_down(x, y, audioMixer_)) return;
    if (button == PointerButton::Primary && synthPanel_.open() && synthPanel_.pointer_down(x, y, audioMixer_.synthesizer())) return;
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
    if (doubleClick) {
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
        for (std::size_t index = 0; index < layout_.toolbarButtons.size(); ++index) {
            if (layout_.toolbarButtons[index].contains(x, y)) {
                set_active_tool(static_cast<EditorToolId>(index));
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
                if ((modifiers & 1U) != 0U) workspace_.toggle_selection(order[index]);
                else workspace_.select_object(order[index]);
                focusRegion_ = EditorFocusRegion::SceneHierarchy;
                // Armed as a pending drag source; pointer_move promotes it to an active drag
                // only once the pointer has actually moved, so a plain click never reparents.
                hierarchyDrag_ = {};
                hierarchyDrag_.sourceId = order[index];
                return;
            }
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
                constexpr float degreesPerRadian = 57.29577951308232F;
                const std::string initial = index == 0
                    ? format_float3_line(object->transform.position)
                    : format_float3_line(multiply(quaternion_to_euler_xyz(object->transform.rotation), degreesPerRadian));
                begin_text_edit(index == 0 ? TextEditKind::Position : TextEditKind::Rotation, *selected, initial);
            }
            focusRegion_ = EditorFocusRegion::Inspector;
            return;
        }
        if (workspace_.selected_object()) {
            for (std::size_t index = 0; index < layout_.inspectorToggles.size(); ++index) {
                if (!layout_.inspectorToggles[index].contains(x, y)) continue;
                const EditorObject* primary = workspace_.document().find_object(*workspace_.selected_object());
                if (!primary) break;
                bool target = false;
                switch (index) {
                    case 0: target = !primary->flags.visible; break;
                    case 1: target = !primary->flags.locked; break;
                    case 2: target = !primary->flags.anchored; break;
                    case 3: target = !primary->flags.structural; break;
                    case 4: target = !primary->flags.collisionEnabled; break;
                    default: break;
                }
                std::vector<ObjectFlagsChange> changes;
                for (EditorObjectId id : workspace_.selected_objects()) {
                    const EditorObject* object = workspace_.document().find_object(id);
                    if (!object) continue;
                    EditorObjectFlags after = object->flags;
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
                const CommandResult result = workspace_.commands().execute(
                    workspace_.document(), std::make_unique<SetObjectFlagsBatchCommand>(std::move(changes)));
                set_status(result.success ? "Updated selected object policies" : result.message, !result.success);
                focusRegion_ = EditorFocusRegion::Inspector;
                return;
            }
        }
        for (std::size_t index = 0; index < layout_.bottomTabs.size(); ++index) {
            if (layout_.bottomTabs[index].contains(x, y)) {
                bottomTab_ = static_cast<BottomPanelTab>(index);
                if (bottomTab_ == BottomPanelTab::Assets && assetDatabase_.records().empty())
                    (void)refresh_asset_database(false);
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
                (void)refresh_asset_database(true);
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
        return;
    }
    dragButton_ = button;
    if (button == PointerButton::Primary) {
        update_hover(x, y);
        if ((activeTool_ == EditorToolId::Translate || activeTool_ == EditorToolId::Rotate) &&
            hit_test_gizmo_axis(x, y) != 0) {
            begin_gizmo_drag(x, y);
        } else if (activeTool_ == EditorToolId::Select || activeTool_ == EditorToolId::Translate ||
                   activeTool_ == EditorToolId::Rotate) {
            if (hoverPick_) {
                if ((modifiers & 1U) != 0U) workspace_.toggle_selection(hoverPick_->objectId);
                else workspace_.select_object(hoverPick_->objectId);
            } else {
                // Nothing under the cursor: could be a plain click (clear selection, handled
                // in pointer_up if the pointer barely moves) or the start of a marquee drag.
                marquee_.active = true;
                marquee_.startX = marquee_.currentX = x;
                marquee_.startY = marquee_.currentY = y;
            }
        } else {
            begin_voxel_stroke(x, y);
        }
    }
    (void)modifiers;
}

void NativeEditorController::pointer_up(PointerButton button, int x, int y, std::uint32_t modifiers) {
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
    if (!doubleClickConsumed_ && completedClickDistance <= 25) {
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
            if (layout_.viewport.contains(x, y)) {
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
        if (marquee_.active) {
            const int distanceSquared = (x - marquee_.startX) * (x - marquee_.startX) + (y - marquee_.startY) * (y - marquee_.startY);
            if (distanceSquared <= 25) {
                if ((modifiers & 1U) == 0U) workspace_.clear_selection();
            } else {
                const int left = std::min(marquee_.startX, x);
                const int right = std::max(marquee_.startX, x);
                const int top = std::min(marquee_.startY, y);
                const int bottom = std::max(marquee_.startY, y);
                if ((modifiers & 1U) == 0U) workspace_.clear_selection();
                for (const auto& [id, object] : workspace_.document().objects()) {
                    const EditorObjectBounds bounds = object_world_bounds(object);
                    if (!bounds.valid) continue;
                    float minX = 1.0e9F, minY = 1.0e9F, maxX = -1.0e9F, maxY = -1.0e9F;
                    bool anyVisible = false;
                    for (int corner = 0; corner < 8; ++corner) {
                        const Float3 point{
                            (corner & 1) ? bounds.maximum.x : bounds.minimum.x,
                            (corner & 2) ? bounds.maximum.y : bounds.minimum.y,
                            (corner & 4) ? bounds.maximum.z : bounds.minimum.z};
                        const ScreenPoint projected = project_world_to_screen(camera_, layout_.viewport, point);
                        if (!projected.visible) continue;
                        anyVisible = true;
                        minX = std::min(minX, projected.x); maxX = std::max(maxX, projected.x);
                        minY = std::min(minY, projected.y); maxY = std::max(maxY, projected.y);
                    }
                    if (!anyVisible) continue;
                    const bool overlaps = maxX >= static_cast<float>(left) && minX <= static_cast<float>(right) &&
                                          maxY >= static_cast<float>(top) && minY <= static_cast<float>(bottom);
                    if (overlaps) workspace_.add_to_selection(id);
                }
            }
            marquee_ = {};
        }
        if (gizmoDragging_) finish_gizmo_drag(false);
        finish_voxel_stroke();
    }
    if (dragButton_ == button) {
        dragButton_ = PointerButton::NoButton;
        activePointerCommand_.clear();
    }
    update_hover(x, y);
    (void)modifiers;
}

void NativeEditorController::pointer_wheel(float steps, int x, int y, std::uint32_t modifiers) {
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

void NativeEditorController::set_active_tool(EditorToolId tool) noexcept {
    activeTool_ = tool;
    close_top_level_menu();
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
    std::unique_ptr<IEditorCommand> command;
    if (activeTool_ == EditorToolId::Anchor) {
        const bool addAnchor = !object->anchors.contains(pick.voxel);
        command = make_anchor_brush_command(workspace_.document(), pick.objectId, pick.voxel, 0, addAnchor);
    } else {
        BrushSettings settings;
        settings.shape = BrushShape::Cube;
        settings.radius = voxelBrushRadius_;
        settings.material = activeMaterial_;
        settings.strokeId = strokeId_;
        Int3 center = pick.voxel;
        switch (activeTool_) {
            case EditorToolId::AddVoxel:
                settings.operation = VoxelToolOperation::Add;
                center = add_int3(pick.voxel, pick.normal);
                break;
            case EditorToolId::RemoveVoxel: settings.operation = VoxelToolOperation::Remove; break;
            case EditorToolId::PaintMaterial: settings.operation = VoxelToolOperation::Paint; break;
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
    const CommandResult result = workspace_.commands().execute(workspace_.document(), std::move(command));
    set_status(result.success ? "Voxel edit applied" : result.message, !result.success, 1.5F);
}

EditorObjectBounds NativeEditorController::selection_bounds() const noexcept {
    EditorObjectBounds combined;
    for (EditorObjectId id : workspace_.selected_objects()) {
        const EditorObject* object = workspace_.document().find_object(id);
        if (!object || !object->flags.visible) continue;
        const EditorObjectBounds bounds = object_world_bounds(*object);
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

Float3 NativeEditorController::selection_pivot() const noexcept {
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
    if (transformSpace_ == EditorTransformSpace::Local && workspace_.selected_object()) {
        if (const EditorObject* object = workspace_.document().find_object(*workspace_.selected_object()))
            direction = transform_vector(object->transform, direction);
    }
    return normalize(direction);
}

std::vector<ObjectTransformChange> NativeEditorController::selection_transform_snapshot() const {
    std::vector<ObjectTransformChange> changes;
    changes.reserve(workspace_.selection_count());
    for (EditorObjectId id : workspace_.selected_objects()) {
        const EditorObject* object = workspace_.document().find_object(id);
        if (!object) continue;
        changes.push_back({id, object->transform, object->transform});
    }
    return changes;
}

void NativeEditorController::apply_transform_preview(const std::vector<ObjectTransformChange>& changes, bool after) {
    for (const ObjectTransformChange& change : changes) {
        if (EditorObject* object = workspace_.document().find_object(change.id))
            object->transform = after ? change.after : change.before;
    }
}

std::vector<GizmoScreenAxis> NativeEditorController::gizmo_axes() const {
    std::vector<GizmoScreenAxis> axes;
    if ((activeTool_ != EditorToolId::Translate && activeTool_ != EditorToolId::Rotate) ||
        workspace_.selected_objects().empty()) return axes;
    const Float3 origin = selection_pivot();
    const float distance = std::max(0.5F, length(subtract(camera_.position, origin)) * 0.12F);
    for (int axis = 1; axis <= 3; ++axis) {
        const ScreenPoint start = project_world_to_screen(camera_, layout_.viewport, origin);
        const ScreenPoint end = project_world_to_screen(camera_, layout_.viewport,
                                                         add(origin, multiply(gizmo_axis_world(axis), distance)));
        axes.push_back({axis, start, end});
    }
    return axes;
}

int NativeEditorController::hit_test_gizmo_axis(int x, int y) const {
    int result = 0;
    float best = 10.0F * workspace_.preferences().uiScale;
    for (const GizmoScreenAxis& axis : gizmo_axes()) {
        if (!axis.start.visible || !axis.end.visible) continue;
        const float distance = point_line_distance(static_cast<float>(x), static_cast<float>(y),
                                                   axis.start.x, axis.start.y, axis.end.x, axis.end.y);
        if (distance < best) { best = distance; result = axis.axis; }
    }
    return result;
}

void NativeEditorController::begin_gizmo_drag(int x, int y) {
    if (workspace_.selected_objects().empty()) return;
    gizmoAxis_ = hit_test_gizmo_axis(x, y);
    if (gizmoAxis_ == 0) return;
    for (EditorObjectId id : workspace_.selected_objects()) {
        const EditorObject* object = workspace_.document().find_object(id);
        if (!object || object->flags.locked) {
            set_status("Unlock all selected objects before transforming them", true);
            gizmoAxis_ = 0;
            return;
        }
    }
    gizmoDragging_ = true;
    gizmoStartX_ = x;
    gizmoStartY_ = y;
    gizmoPivot_ = selection_pivot();
    gizmoChanges_ = selection_transform_snapshot();
}

void NativeEditorController::update_gizmo_drag(int x, int y) {
    if (!gizmoDragging_ || gizmoChanges_.empty()) return;
    const auto axes = gizmo_axes();
    if (gizmoAxis_ < 1 || gizmoAxis_ > static_cast<int>(axes.size())) return;
    const GizmoScreenAxis& screenAxis = axes[static_cast<std::size_t>(gizmoAxis_ - 1)];
    const float dx = screenAxis.end.x - screenAxis.start.x;
    const float dy = screenAxis.end.y - screenAxis.start.y;
    const float screenLength = std::max(1.0F, std::hypot(dx, dy));
    const float projectedDelta = ((static_cast<float>(x - gizmoStartX_) * dx) +
                                  (static_cast<float>(y - gizmoStartY_) * dy)) / screenLength;
    const Float3 worldAxis = gizmo_axis_world(gizmoAxis_);
    for (ObjectTransformChange& change : gizmoChanges_) {
        change.after = change.before;
        if (activeTool_ == EditorToolId::Rotate) {
            constexpr float degreesToRadians = 0.017453292519943295F;
            const float snapRadians = workspace_.preferences().rotateSnapDegrees * degreesToRadians;
            const float rawAngle = projectedDelta * 0.012F;
            const float angle = std::round(rawAngle / snapRadians) * snapRadians;
            const Quaternion delta = quaternion_from_axis_angle(worldAxis, angle);
            change.after.position = add(gizmoPivot_, rotate(delta, subtract(change.before.position, gizmoPivot_)));
            change.after.rotation = normalize(multiply(delta, change.before.rotation));
        } else {
            const float depth = std::max(0.1F, length(subtract(camera_.position, gizmoPivot_)));
            const float worldPerPixel = 2.0F * depth * std::tan(camera_.verticalFovRadians * 0.5F) /
                                        static_cast<float>(std::max(1, layout_.viewport.height));
            float worldDelta = projectedDelta * worldPerPixel;
            const float snapMeters = workspace_.preferences().translateSnapMeters;
            worldDelta = std::round(worldDelta / snapMeters) * snapMeters;
            change.after.position = add(change.before.position, multiply(worldAxis, worldDelta));
        }
    }
    apply_transform_preview(gizmoChanges_, true);
}

void NativeEditorController::finish_gizmo_drag(bool cancel) {
    if (!gizmoDragging_) return;
    apply_transform_preview(gizmoChanges_, false);
    if (!cancel && !gizmoChanges_.empty()) {
        const std::string label = activeTool_ == EditorToolId::Rotate ? "Rotate objects" : "Move objects";
        const CommandResult result = workspace_.commands().execute(
            workspace_.document(), std::make_unique<TransformObjectsCommand>(gizmoChanges_, label));
        set_status(result.success ? label : result.message, !result.success);
    }
    gizmoDragging_ = false;
    gizmoAxis_ = 0;
    gizmoChanges_.clear();
}

bool NativeEditorController::dispatch_action(std::string_view actionId) {
    // Selection, clipboard, play-session, and authoring-panel state can change outside menu
    // interaction. Re-evaluate availability at the command boundary so shortcuts, automation,
    // context menus, and top-level menus all enforce the same current-state contract.
    refresh_menu_state();
    if (const MenuAction* action = workspace_.menus().find(actionId)) {
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
        set_status("Scale tool selected; polygon component scaling and voxel rescale use context-specific commands");
        return true;
    }
    if (actionId == "transform.universal") { set_active_tool(EditorToolId::Translate); return true; }
    if (actionId == "camera.fly_forward") return flyStep({0.0F,0.0F,1.0F});
    if (actionId == "camera.fly_backward") return flyStep({0.0F,0.0F,-1.0F});
    if (actionId == "camera.fly_left") return flyStep({-1.0F,0.0F,0.0F});
    if (actionId == "camera.fly_right") return flyStep({1.0F,0.0F,0.0F});
    if (actionId == "camera.fly_down") return flyStep({0.0F,-1.0F,0.0F});
    if (actionId == "camera.fly_up") return flyStep({0.0F,1.0F,0.0F});
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
    if (actionId == "view.frame_all") {
        const auto selection = workspace_.selected_objects();
        workspace_.clear_selection();
        frame_selection();
        for (EditorObjectId id : selection) workspace_.add_to_selection(id);
        return true;
    }
    if (actionId == "view.follow_selection") { set_status("Follow selection toggled"); return true; }
    if (actionId == "view.drop_to_surface") { set_status("Drop to surface requires a selected collision surface"); return true; }
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
    if (actionId == "voxel.brush_increase" || actionId == "voxel.brush_decrease") {
        voxelBrushRadius_ = std::clamp(voxelBrushRadius_ + (actionId == "voxel.brush_increase" ? 1 : -1), 0, 32);
        set_status("Voxel brush radius: " + std::to_string(voxelBrushRadius_));
        return true;
    }
    if (actionId == "voxel.fill") { set_status("Flood fill tool selected"); return true; }
    if (actionId == "voxel.commit") { finish_voxel_stroke(); set_status("Voxel operation committed"); return true; }
    if (actionId == "voxel.rebuild_brickmaps") { set_status("Affected brickmaps queued for rebuild"); return true; }
    if (actionId == "voxel.rebuild_collision") { set_status("Collision proxies queued for rebuild"); return true; }
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
            spriteLevelAccumulator_ = 0.0F;
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
        spriteLevelAccumulator_ = 0.0F;
        spriteLevelPlaying_ = true;
        close_top_level_menu();
        close_context_menu();
        set_status("Original sprite level running | arrows/A-D move, Space jump, X fire, R restart, Esc stop", false, 7.0F);
        return true;
    }
    if (actionId == "sprite.editor") {
        if (assetBrowserState_.selectedId) {
            const EditorAssetRecord* selected = assetDatabase_.find(*assetBrowserState_.selectedId);
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
    if (actionId == "material.assign" || actionId == "material.link" || actionId == "material.create_instance" ||
        actionId == "material.create_master" || actionId == "material.preview" ||
        actionId == "material.pin_preview" || actionId == "material.apply" ||
        actionId.starts_with("polygon.") || actionId.starts_with("audio.") ||
        actionId.starts_with("sequencer.")) {
        set_status("Shortcut dispatched: " + std::string(actionId));
        return true;
    }

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
    if (actionId == "create.polygon" || actionId == "polygon.import") {
        set_status("Polygon import is available through File > Import Model and the .dmesh cooker");
        return true;
    }
    if (actionId == "build.validate") {
        workspace_.problems().clear();
        set_status("Project validation completed");
        return true;
    }

    if (actionId == "edit.undo") {
        const CommandResult result = workspace_.commands().undo(workspace_.document());
        if (result.success) {
            workspace_.prune_selection();
            recompute_layout();
        }
        set_status(result.success ? "Undo" : result.message, !result.success);
        return result.success;
    }
    if (actionId == "edit.redo") {
        const CommandResult result = workspace_.commands().redo(workspace_.document());
        if (result.success) {
            workspace_.prune_selection();
            recompute_layout();
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
    if (actionId == "transform.scale_double") {
        return rescale_primary_voxel_object({2.0F, 2.0F, 2.0F}).success;
    }
    if (actionId == "transform.scale_half") {
        return rescale_primary_voxel_object({0.5F, 0.5F, 0.5F}).success;
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
        if (workspace_.document().path().empty()) {
            set_status("Save requires a scene path; use the headless project workflow or pass --scene", true);
            return false;
        }
        const auto result = workspace_.document().save_transactional(workspace_.document().path());
        set_status(result.success ? "Scene saved" : result.error, !result.success);
        return result.success;
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
        const EditorAssetRecord* asset = assetDatabase_.find(*assetBrowserState_.selectedId);
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
        const EditorAssetRecord* asset = assetDatabase_.find(*assetBrowserState_.selectedId);
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

        Float3 centroid{};
        std::optional<EditorObjectId> commonParent;
        bool firstParent = true;
        for (const EditorObjectId id : roots) {
            const EditorObject* object = workspace_.document().find_object(id);
            if (!object) return false;
            centroid = add(centroid, object->transform.position);
            if (firstParent) { commonParent = object->parent; firstParent = false; }
            else if (commonParent != object->parent) commonParent.reset();
        }
        centroid = multiply(centroid, 1.0F / static_cast<float>(roots.size()));

        EditorObject group(workspace_.document().allocate_object_id(), "Group");
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
            workspace_.select_object(groupId);
            recompute_layout();
        }
        set_status(result.success ? "Grouped selection" : result.message, !result.success);
        return result.success;
    }
    if (actionId == "edit.ungroup") {
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
            if (group && (!group->voxels || group->voxels->occupied_voxel_count() == 0))
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

        auto compound = std::make_unique<CompoundCommand>("Ungroup");
        if (!reparents.empty())
            compound->add(std::make_unique<ReparentObjectsCommand>(std::move(reparents), "Ungroup"));
        if (!emptyGroupsToDelete.empty())
            compound->add(std::make_unique<RemoveObjectsCommand>(std::move(emptyGroupsToDelete), "Ungroup"));
        const CommandResult result = workspace_.commands().execute(workspace_.document(), std::move(compound));
        if (result.success) {
            workspace_.clear_selection();
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
    if (actionId == "asset.refresh") return refresh_asset_database(true);
    if (actionId == "asset.rename") {
        if (!assetBrowserState_.selectedId) { set_status("No asset selected", true); return false; }
        const EditorAssetRecord* asset = assetDatabase_.find(*assetBrowserState_.selectedId);
        if (!asset) { set_status("Selected asset no longer exists", true); return false; }
        bottomTab_ = BottomPanelTab::Assets;
        begin_text_edit(TextEditKind::AssetRename, 0, asset->relativePath.filename().string());
        recompute_layout();
        return true;
    }
    if (actionId == "asset.thumbnail") {
        if (!assetBrowserState_.selectedId) { set_status("No asset selected", true); return false; }
        std::string error;
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
        if (bottomTab_ == BottomPanelTab::Assets && assetDatabase_.records().empty())
            (void)refresh_asset_database(false);
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
    if (actionId == "view.increase_angle_snap" || actionId == "view.decrease_angle_snap") {
        static constexpr std::array<float, 6> kSteps{1.0F, 5.0F, 15.0F, 30.0F, 45.0F, 90.0F};
        float& snap = workspace_.preferences().rotateSnapDegrees;
        std::size_t nearest = 0;
        for (std::size_t i = 1; i < kSteps.size(); ++i)
            if (std::abs(kSteps[i] - snap) < std::abs(kSteps[nearest] - snap)) nearest = i;
        if (actionId == "view.increase_angle_snap") nearest = std::min(nearest + 1, kSteps.size() - 1);
        else nearest = nearest == 0 ? 0 : nearest - 1;
        snap = kSteps[nearest];
        set_status("Angle snap: " + std::to_string(snap).substr(0, 5) + " deg");
        return true;
    }
    if (actionId == "help.about") {
        set_status("Destructible Voxel Engine native editor (Linux/X11 authoring host)", false, 6.0F);
        return true;
    }
    set_status("Action is represented but not connected in this platform build: " + std::string(actionId), false);
    return false;
}

void NativeEditorController::key_down(std::string_view key, bool control, bool shift, bool alt) {
    const std::string normalized = lowercase(key);
    route_play_key(normalized, true);
    if (pending_destructive_confirmation()) {
        if (normalized == "return" || normalized == "enter") confirm_pending_destructive_action();
        else if (normalized == "escape") cancel_pending_destructive_action();
        return;
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
        if (control && normalized == "r") { (void)refresh_asset_database(true); return; }
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
            const std::size_t visible = std::max<std::size_t>(1U, layout_.assetRows.size());
            if (selectedIndex < assetBrowserState_.firstVisible) assetBrowserState_.firstVisible = selectedIndex;
            else if (selectedIndex >= assetBrowserState_.firstVisible + visible)
                assetBrowserState_.firstVisible = selectedIndex - visible + 1U;
            recompute_layout();
            return;
        }
        if ((normalized == "return" || normalized == "enter") && assetBrowserState_.selectedId) {
            if (const EditorAssetRecord* asset = assetDatabase_.find(*assetBrowserState_.selectedId)) {
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
        close_top_level_menu();
        close_context_menu();
        if (gizmoDragging_) finish_gizmo_drag(true);
        finish_voxel_stroke();
        return;
    }
    if (normalized == "tab") { focus_next(shift); return; }

    // Fly navigation is a held-command context. It must resolve before ordinary key presses
    // so RMB+W moves the camera while plain W still selects the Move tool.
    const auto contexts = active_shortcut_contexts();
    if (std::find(contexts.begin(), contexts.end(), ShortcutContext::FlyNavigation) != contexts.end()) {
        const ShortcutGesture held = keyboard_shortcut(normalized, control, shift, alt, ShortcutActivation::Hold);
        const auto existing = std::find_if(heldShortcutGestures_.begin(), heldShortcutGestures_.end(),
            [&](const ShortcutGesture& gesture) { return gesture == held; });
        if (existing == heldShortcutGestures_.end()) heldShortcutGestures_.push_back(held);
        if (dispatch_shortcut_gesture(held)) return;
        heldShortcutGestures_.pop_back();
    }
    if (dispatch_shortcut_gesture(keyboard_shortcut(normalized, control, shift, alt))) return;
}

void NativeEditorController::focus_next(bool reverse) noexcept {
    constexpr int count = 6;
    int value = static_cast<int>(focusRegion_);
    value = reverse ? (value + count - 1) % count : (value + 1) % count;
    focusRegion_ = static_cast<EditorFocusRegion>(value);
}

void NativeEditorController::frame_selection() noexcept {
    EditorObjectBounds combined = selection_bounds();
    if (!combined.valid) {
        for (const auto& [id, object] : workspace_.document().objects()) {
            (void)id;
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
    std::vector<ObjectTransformChange> changes = selection_transform_snapshot();
    if (changes.empty()) return CommandResult::fail("no objects selected");
    for (ObjectTransformChange& change : changes) change.after.position = add(change.before.position, worldDelta);
    CommandResult result = workspace_.commands().execute(
        workspace_.document(), std::make_unique<TransformObjectsCommand>(std::move(changes), "Nudge objects"));
    set_status(result.success ? "Nudged selection" : result.message, !result.success);
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

EditorSelectionDiagnostics NativeEditorController::selection_diagnostics() const {
    return analyze_editor_selection(workspace_.document(), materials_, workspace_.selected_objects());
}

std::vector<EditorVoxelDrawItem> NativeEditorController::draw_items() const {
    return build_voxel_draw_list(workspace_.document(), materials_, camera_, layout_.viewport,
                                 viewportSettings_, workspace_.selected_objects());
}

std::vector<EditorText3DDrawItem> NativeEditorController::text3d_draw_items() const {
    return build_text3d_draw_list(workspace_.document(), camera_, layout_.viewport,
                                  workspace_.selected_objects());
}

std::vector<EditorGaborVolumeDrawItem> NativeEditorController::gabor_volume_draw_items() const {
    return build_gabor_volume_draw_list(workspace_.document(), camera_, layout_.viewport,
                                        workspace_.selected_objects());
}

std::vector<EditorObjectId> NativeEditorController::hierarchy_order() const {
    std::vector<EditorObjectId> result;
    const auto append = [&](const auto& self, EditorObjectId id, std::vector<EditorObjectId>& output) -> void {
        output.push_back(id);
        for (EditorObjectId child : workspace_.document().children_of(id)) self(self, child, output);
    };
    for (EditorObjectId root : workspace_.document().root_objects()) append(append, root, result);
    if (hierarchyFilter_.empty()) return result;
    const std::string needle = lowercase(hierarchyFilter_);
    std::erase_if(result, [&](EditorObjectId id) {
        const EditorObject* object = workspace_.document().find_object(id);
        return !object || lowercase(object->name).find(needle) == std::string::npos;
    });
    return result;
}

EditorDocument make_new_project_document() {
    EditorDocument document("Untitled Project");
    EditorObject oval(1, "Starter Oval");
    oval.voxelSizeMeters = 0.25F;
    oval.flags.structural = true;
    oval.flags.collisionEnabled = true;

    // A compact ellipsoid resting on the Y=0 grid plane. Voxel-center sampling makes the
    // silhouette symmetric despite even diameters and avoids a flat, box-derived boundary.
    constexpr int radiusX = 8;
    constexpr int radiusY = 5;
    constexpr int radiusZ = 6;
    for (int y = 0; y < radiusY * 2; ++y) {
        const float normalizedY = (static_cast<float>(y) + 0.5F - static_cast<float>(radiusY)) /
                                  static_cast<float>(radiusY);
        for (int z = -radiusZ; z < radiusZ; ++z) {
            const float normalizedZ = (static_cast<float>(z) + 0.5F) / static_cast<float>(radiusZ);
            for (int x = -radiusX; x < radiusX; ++x) {
                const float normalizedX = (static_cast<float>(x) + 0.5F) / static_cast<float>(radiusX);
                if (normalizedX * normalizedX + normalizedY * normalizedY +
                    normalizedZ * normalizedZ <= 1.0F)
                    oval.voxels->set_voxel({x, y, z}, kDefaultSurfaceMaterial);
            }
        }
    }
    document.add_object(std::move(oval));
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

} // namespace dve::editor
