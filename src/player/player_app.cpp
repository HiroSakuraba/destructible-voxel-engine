#include "dve/player/player_app.hpp"

#include "dve/game_scene_loader.hpp"

#if defined(DVE_HAVE_LUA)
#include "dve/game_script.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace dve::player {

std::uint32_t FixedStepClock::advance(double elapsedSeconds) noexcept {
    if (!(elapsedSeconds > 0.0) || !std::isfinite(elapsedSeconds)) return 0U;
    accumulator += std::min(elapsedSeconds, maximumFrameSeconds);
    const double step = static_cast<double>(fixedDeltaSeconds);
    std::uint32_t steps = 0U;
    while (accumulator >= step && steps < maximumSteps) {
        accumulator -= step;
        ++steps;
    }
    if (steps == maximumSteps) accumulator = std::min(accumulator, step); // drop the backlog
    return steps;
}

std::filesystem::path user_data_directory() {
    const auto env = [](const char* name) -> std::string {
        const char* value = std::getenv(name);
        return value ? std::string(value) : std::string();
    };
#if defined(_WIN32)
    if (const std::string appData = env("APPDATA"); !appData.empty()) return std::filesystem::path(appData);
    if (const std::string profile = env("USERPROFILE"); !profile.empty())
        return std::filesystem::path(profile) / "AppData" / "Roaming";
    return {};
#else
    const std::string home = env("HOME");
#if defined(__APPLE__)
    if (!home.empty()) return std::filesystem::path(home) / "Library" / "Application Support";
    return {};
#else
    // XDG Base Directory spec: relative values are invalid and must be ignored.
    if (const std::string xdg = env("XDG_DATA_HOME"); !xdg.empty() && std::filesystem::path(xdg).is_absolute())
        return std::filesystem::path(xdg);
    if (!home.empty()) return std::filesystem::path(home) / ".local" / "share";
    return {};
#endif
#endif
}

std::string save_directory_slug(std::string_view gameName) {
    std::string slug;
    for (const char raw : gameName) {
        const auto c = static_cast<unsigned char>(raw);
        if (std::isalnum(c) != 0 && c < 0x80U) slug.push_back(static_cast<char>(std::tolower(c)));
        else if ((c == '_' || c == '.') && !slug.empty()) slug.push_back(static_cast<char>(c));
        else if (!slug.empty() && slug.back() != '-') slug.push_back('-');
        if (slug.size() >= 64U) break;
    }
    while (!slug.empty() && (slug.back() == '-' || slug.back() == '.')) slug.pop_back();
    return slug.empty() ? std::string("game") : slug;
}

std::filesystem::path default_save_directory(const GameManifest& manifest) {
    const std::string slug = save_directory_slug(manifest.name);
    const std::filesystem::path data = user_data_directory();
    if (data.empty()) return std::filesystem::path("saves") / slug;
    return data / "dve" / slug / "saves";
}

bool valid_save_slot_name(std::string_view slot) noexcept {
    if (slot.empty() || slot.size() > kMaximumSaveSlotNameBytes || slot.front() == '.') return false;
    return std::all_of(slot.begin(), slot.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return (u < 0x80U && std::isalnum(u) != 0) || c == '_' || c == '-' || c == '.';
    });
}

namespace {

// One booted world with its scene and script host. Declaration order is destruction order in
// reverse: the script host (which holds listeners into the world) goes before the world.
struct PlayerSession {
    std::unique_ptr<GameWorld> world;
    std::string physicsBackend;
#if defined(DVE_HAVE_LUA)
    std::unique_ptr<GameScriptHost> script;
#endif
    bool scriptLoaded{};
    std::string scriptPath;
    std::string scenePath;
    std::string sceneName;
    std::size_t sceneObjects{};

    PlayerSession() = default;
    PlayerSession(PlayerSession&&) noexcept = default;
    PlayerSession& operator=(PlayerSession&& other) noexcept {
        if (this != &other) {
            release();
            world = std::move(other.world);
            physicsBackend = std::move(other.physicsBackend);
#if defined(DVE_HAVE_LUA)
            script = std::move(other.script);
#endif
            scriptLoaded = other.scriptLoaded;
            scriptPath = std::move(other.scriptPath);
            scenePath = std::move(other.scenePath);
            sceneName = std::move(other.sceneName);
            sceneObjects = other.sceneObjects;
        }
        return *this;
    }
    ~PlayerSession() { release(); }
    void release() noexcept {
#if defined(DVE_HAVE_LUA)
        script.reset();
#endif
        world.reset();
    }
};

struct PendingSaveRequest {
    bool load{};
    std::string slot;
    bool fromScript{};
};

} // namespace

struct PlayerApp::Impl {
    // The content source outlives the session (the script host holds a pointer to it).
    std::unique_ptr<ContentSource> content;
    GameManifest manifest;
    ui::GameSettings settings;
    PlayerInput input;
    PlayerBootOptions options;
    std::filesystem::path saveDirectory;
    std::unique_ptr<GameSaveCodec> codec;
    std::vector<PendingSaveRequest> pending;
    std::vector<PlayerSaveEvent> saveEvents;
    bool quicksaveHeld{};
    bool quickloadHeld{};
    bool quit{};
    std::uint64_t ticks{};
    PlayerSession session;

    void log(std::string_view line) const {
        if (options.log) options.log(line);
    }
    [[nodiscard]] bool build_session(PlayerSession& out, const std::string& scenePath, bool logSteps, std::string* error);
};

bool PlayerApp::Impl::build_session(PlayerSession& out, const std::string& scenePath, bool logSteps, std::string* error) {
    const auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    std::string stepError;
    // 4. World + physics: Jolt when compiled in, otherwise the reference solver.
    Physics3DBackend resolved = Physics3DBackend::Reference;
    auto physics = create_physics3d_world(options.physicsBackend, &resolved, &stepError);
    if (!physics) return fail("physics: " + stepError);
    out.physicsBackend = std::string(physics3d_backend_label(resolved));
    out.world = std::make_unique<GameWorld>(std::move(physics));
    if (logSteps) log("physics: " + out.physicsBackend);

    // 5. Scene.
    out.scenePath = scenePath;
    GameSceneLoadOptions sceneOptions;
    sceneOptions.attachChildrenToParents = options.attachChildrenToParents;
    const GameSceneLoadResult scene = load_scene_into_game_world(*content, out.scenePath, *out.world, sceneOptions);
    if (!scene) return fail("scene '" + out.scenePath + "': " + scene.error.message);
    out.sceneName = scene.sceneName;
    out.sceneObjects = scene.objects.size();
    std::size_t attached = 0;
    for (const GameSceneLoadedObject& object : scene.objects) attached += object.attached ? 1U : 0U;
    if (logSteps)
        log("scene: " + out.sceneName + " (" + std::to_string(out.sceneObjects) + " objects, " +
            std::to_string(attached) + " attached)");

    // 6. Scripts (after the scene so the startup script can find scene objects).
    const std::string scriptPath = manifest.startup_script_or_default();
    const bool scriptPresent = content->exists(scriptPath);
#if defined(DVE_HAVE_LUA)
    if (options.enableScripts) {
        out.script = std::make_unique<GameScriptHost>(*out.world);
        out.script->set_content_source(content.get());
        out.script->set_log_sink([callback = options.log](bool isError, std::string message) {
            if (callback) callback(std::string(isError ? "lua error: " : "lua: ") + message);
        });
        out.script->set_save_request_handler([this](ScriptSaveRequest kind, const std::string& slot, std::string* message) {
            if (!valid_save_slot_name(slot)) {
                if (message) *message = "invalid save slot name '" + slot + "' (use 1-64 of A-Z a-z 0-9 _ - .)";
                return false;
            }
            if (kind == ScriptSaveRequest::Exists) {
                std::error_code ec;
                return std::filesystem::is_regular_file(saveDirectory / (slot + std::string(kGameSaveExtension)), ec);
            }
            pending.push_back({kind == ScriptSaveRequest::Load, slot, true});
            return true;
        });
        if (scriptPresent) {
            if (!out.script->run_content_file(scriptPath, &stepError))
                return fail("script '" + scriptPath + "': " + stepError);
            out.scriptLoaded = true;
            out.scriptPath = scriptPath;
            if (logSteps) log("script: " + scriptPath);
        } else if (manifest.startupScript) {
            return fail("script '" + scriptPath + "' is missing");
        }
    } else if (logSteps) {
        log("script: disabled");
    }
#else
    if (scriptPresent && logSteps) log("script: " + scriptPath + " skipped (built without Lua)");
#endif
    return true;
}

PlayerApp::PlayerApp() : impl_(std::make_unique<Impl>()) {}
PlayerApp::~PlayerApp() = default;

std::unique_ptr<PlayerApp> PlayerApp::boot(
    std::unique_ptr<ContentSource> content, const PlayerBootOptions& options, std::string* error) {
    const auto fail = [&](std::string message) -> std::unique_ptr<PlayerApp> {
        if (error) *error = std::move(message);
        return nullptr;
    };
    const auto log = [&](std::string_view line) {
        if (options.log) options.log(line);
    };
    if (!content) return fail("no game content");
    std::unique_ptr<PlayerApp> app(new PlayerApp());
    Impl& impl = *app->impl_;
    impl.content = std::move(content);
    log("content: " + impl.content->describe());

    // 1. Manifest.
    std::string stepError;
    auto manifest = load_game_manifest(*impl.content, &stepError);
    if (!manifest) return fail("game.dvegame: " + stepError);
    if (!check_game_manifest_content(*manifest, *impl.content, &stepError))
        return fail("game.dvegame: " + stepError);
    impl.manifest = std::move(*manifest);
    log("game: " + impl.manifest.name + " " + impl.manifest.version);

    // 2. Settings (optional file; engine defaults otherwise).
    if (impl.manifest.settings) {
        ContentError readError;
        const auto text = impl.content->read_text(*impl.manifest.settings, &readError, 64U * 1024U);
        if (!text) return fail("settings '" + *impl.manifest.settings + "': " + readError.message);
        auto settings = ui::GameSettings::parse(*text, &stepError);
        if (!settings || !settings->validate(&stepError))
            return fail("settings '" + *impl.manifest.settings + "': " + stepError);
        impl.settings = std::move(*settings);
    }

    // 3. Input bindings (bind.* entries, decision D6), plus the reserved quicksave /
    //    quickload actions unless the game binds them itself.
    std::map<std::string, std::string> bindingSpecs = impl.manifest.inputBindings;
    bindingSpecs.try_emplace(std::string(kQuicksaveAction), std::string(kDefaultQuicksaveBinding));
    bindingSpecs.try_emplace(std::string(kQuickloadAction), std::string(kDefaultQuickloadBinding));
    auto bindings = InputBindingTable::parse(bindingSpecs, &stepError);
    if (!bindings) return fail("game.dvegame: " + stepError);
    for (const std::string_view reserved : {kQuicksaveAction, kQuickloadAction}) {
        if (const InputBinding* binding = bindings->find(reserved); binding && binding->axis)
            return fail("game.dvegame: bind." + std::string(reserved) + " must be an action, not an axis");
    }
    impl.input = PlayerInput(std::move(*bindings));

    impl.options = options;
    impl.saveDirectory = options.saveDirectory.value_or(default_save_directory(impl.manifest));
    const ContentSource* contentPointer = impl.content.get();
    impl.codec = std::make_unique<GameSaveCodec>(
        [contentPointer](std::string_view path, std::uint64_t maximumBytes, std::string* readError)
            -> std::optional<std::vector<std::byte>> {
            ContentError contentError;
            auto bytes = contentPointer->read(path, &contentError, maximumBytes);
            if (!bytes && readError) *readError = contentError.message.empty() ? "cannot read " + std::string(path) : contentError.message;
            return bytes;
        });

    // 4-6. World + physics, scene, scripts.
    if (!impl.build_session(impl.session, options.sceneOverride.value_or(impl.manifest.entryScene), true, &stepError))
        return fail(stepError);

    // 7. Optional save to resume (--load).
    if (options.loadSave) {
        const auto loaded = app->load_game(*options.loadSave, &stepError);
        if (!loaded) return fail("load '" + *options.loadSave + "': " + stepError);
    }
    return app;
}

const ContentSource& PlayerApp::content() const noexcept { return *impl_->content; }
const GameManifest& PlayerApp::manifest() const noexcept { return impl_->manifest; }
const ui::GameSettings& PlayerApp::settings() const noexcept { return impl_->settings; }
GameWorld& PlayerApp::world() noexcept { return *impl_->session.world; }
const GameWorld& PlayerApp::world() const noexcept { return *impl_->session.world; }
const PlayerInput& PlayerApp::input() const noexcept { return impl_->input; }
std::string_view PlayerApp::physics_backend() const noexcept { return impl_->session.physicsBackend; }
bool PlayerApp::scripting_compiled_in() noexcept {
#if defined(DVE_HAVE_LUA)
    return true;
#else
    return false;
#endif
}
bool PlayerApp::script_loaded() const noexcept { return impl_->session.scriptLoaded; }
const std::string& PlayerApp::script_path() const noexcept { return impl_->session.scriptPath; }
const std::string& PlayerApp::scene_path() const noexcept { return impl_->session.scenePath; }
const std::string& PlayerApp::scene_name() const noexcept { return impl_->session.sceneName; }
std::size_t PlayerApp::scene_object_count() const noexcept { return impl_->session.sceneObjects; }

void PlayerApp::handle_event(const platform::PlatformEvent& event) {
    if (event.type == platform::EventType::QuitRequested) impl_->quit = true;
    impl_->input.handle_event(event);
}

bool PlayerApp::quit_requested() const noexcept { return impl_->quit; }
void PlayerApp::request_quit() noexcept { impl_->quit = true; }

void PlayerApp::tick(float fixedDeltaSeconds) {
    Impl& impl = *impl_;
    impl.saveEvents.clear();
    // Reserved actions fire on the press edge; the save/load itself runs after the step so
    // it never happens halfway through GameWorld::tick or a Lua callback.
    const bool quicksave = impl.input.action(kQuicksaveAction);
    const bool quickload = impl.input.action(kQuickloadAction);
    if (quicksave && !impl.quicksaveHeld) impl.pending.push_back({false, std::string(kQuicksaveSlot), false});
    if (quickload && !impl.quickloadHeld) impl.pending.push_back({true, std::string(kQuicksaveSlot), false});
    impl.quicksaveHeld = quicksave;
    impl.quickloadHeld = quickload;

    impl.input.apply(*impl.session.world);
    impl.session.world->tick(fixedDeltaSeconds);
    ++impl.ticks;

    std::vector<PendingSaveRequest> requests;
    requests.swap(impl.pending);
    for (const PendingSaveRequest& request : requests) {
        PlayerSaveEvent event;
        event.load = request.load;
        event.fromScript = request.fromScript;
        std::string error;
        if (const auto path = resolve_save(request.slot, &error)) event.path = *path;
        if (request.load) {
            if (const auto loaded = load_game(request.slot, &error)) {
                event.ok = true;
                event.fileBytes = loaded->read.fileBytes;
            }
        } else if (const auto saved = save_game(request.slot, &error)) {
            event.ok = true;
            event.fileBytes = saved->stats.fileBytes;
        }
        event.message = event.ok ? std::string() : error;
        impl.log(std::string(request.load ? "load " : "save ") + event.path.string() + ": " +
                 (event.ok ? std::to_string(event.fileBytes) + " bytes" : "failed: " + error));
        impl.saveEvents.push_back(std::move(event));
    }
}

std::uint64_t PlayerApp::tick_count() const noexcept { return impl_->ticks; }

render::PolygonCamera PlayerApp::camera(float aspect) const {
    render::PolygonCamera camera;
    camera.position = {0.0F, 1.5F, 6.0F};
    camera.target = {0.0F, 0.5F, 0.0F};
    if (impl_->manifest.camera) {
        camera.position = impl_->manifest.camera->eye;
        camera.target = impl_->manifest.camera->target;
    }
#if defined(DVE_HAVE_LUA)
    if (impl_->session.script) {
        if (const auto eye = impl_->session.script->global_vector(std::string(kCameraEyeGlobal)))
            camera.position = {eye->x, eye->y, eye->z};
        if (const auto target = impl_->session.script->global_vector(std::string(kCameraTargetGlobal)))
            camera.target = {target->x, target->y, target->z};
    }
#endif
    // ui::GameSettings::fieldOfViewDegrees is horizontal (60..130); the camera wants vertical.
    const float safeAspect = aspect > 0.0F && std::isfinite(aspect) ? aspect : 16.0F / 9.0F;
    const float horizontal = impl_->settings.fieldOfViewDegrees * 0.017453292519943295F;
    camera.verticalFieldOfViewRadians = 2.0F * std::atan(std::tan(horizontal * 0.5F) / safeAspect);
    camera.nearPlane = 0.05F;
    camera.farPlane = 500.0F;
    return camera;
}

RenderEnvironment PlayerApp::environment() const {
#if defined(DVE_HAVE_LUA)
    if (impl_->session.script) return impl_->session.script->environment();
#endif
    return RenderEnvironment{};
}

PlayerRenderView PlayerApp::render_view(std::vector<GameRenderObject>& objects, float aspect) const {
    objects = impl_->session.world->render_objects();
    PlayerRenderView view;
    view.objects = objects;
    view.camera = camera(aspect);
    view.environment = environment();
    view.polygonFrustumCulling = impl_->settings.polygonFrustumCulling;
    view.polygonLodBias = impl_->settings.polygonLodBias;
    return view;
}

std::optional<double> PlayerApp::script_global(const std::string& key) const {
#if defined(DVE_HAVE_LUA)
    if (impl_->session.script) return impl_->session.script->global_number(key);
#else
    (void)key;
#endif
    return std::nullopt;
}

const std::filesystem::path& PlayerApp::save_directory() const noexcept { return impl_->saveDirectory; }
GameSaveCodec& PlayerApp::save_codec() noexcept { return *impl_->codec; }
const std::vector<PlayerSaveEvent>& PlayerApp::save_events() const noexcept { return impl_->saveEvents; }

std::optional<std::filesystem::path> PlayerApp::resolve_save(std::string_view slotOrPath, std::string* error) const {
    const bool looksLikePath = slotOrPath.find('/') != std::string_view::npos ||
                               slotOrPath.find('\\') != std::string_view::npos ||
                               (slotOrPath.size() > kGameSaveExtension.size() && slotOrPath.ends_with(kGameSaveExtension));
    if (looksLikePath) return std::filesystem::path(std::string(slotOrPath));
    if (!valid_save_slot_name(slotOrPath)) {
        if (error) *error = "invalid save slot name '" + std::string(slotOrPath) + "' (use 1-64 of A-Z a-z 0-9 _ - . or a path to a .dvesave)";
        return std::nullopt;
    }
    return impl_->saveDirectory / (std::string(slotOrPath) + std::string(kGameSaveExtension));
}

std::optional<PlayerSaveResult> PlayerApp::save_game(std::string_view slotOrPath, std::string* error) {
    Impl& impl = *impl_;
    const auto path = resolve_save(slotOrPath, error);
    if (!path) return std::nullopt;
    GameSaveData data;
    data.metadata.gameName = impl.manifest.name;
    data.metadata.gameVersion = impl.manifest.version;
    data.metadata.scenePath = impl.session.scenePath;
    data.metadata.tickCount = impl.ticks;
    data.metadata.worldStateHash = impl.session.world->state_hash();
    data.metadata.info["physics"] = impl.session.physicsBackend;
    data.world = impl.session.world->capture_save_state();
    data.runtimes = capture_game_runtime_state(*impl.session.world);
#if defined(DVE_HAVE_LUA)
    if (impl.session.script) {
        std::string scriptError;
        auto scriptState = impl.session.script->save_state(&scriptError);
        if (!scriptState) {
            if (error) *error = "script state: " + scriptError;
            return std::nullopt;
        }
        data.scriptState = std::move(*scriptState);
    }
#endif
    PlayerSaveResult result;
    if (!impl.codec->write_file(*path, data, &result.stats, error)) return std::nullopt;
    result.path = *path;
    result.tickCount = data.metadata.tickCount;
    result.worldStateHash = data.metadata.worldStateHash;
    return result;
}

std::optional<PlayerLoadResult> PlayerApp::load_game(std::string_view slotOrPath, std::string* error) {
    Impl& impl = *impl_;
    const auto fail = [&](std::string message) -> std::optional<PlayerLoadResult> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    const auto path = resolve_save(slotOrPath, error);
    if (!path) return std::nullopt;
    PlayerLoadResult result;
    result.path = *path;
    std::string stepError;
    const auto data = impl.codec->read_file(*path, {}, &result.read, &stepError);
    if (!data) return fail(stepError);
    if (result.read.source == SaveGameReadSource::Backup)
        impl.log("save " + path->string() + " was unreadable (" + result.read.primaryError + "); loaded the backup");
    if (data->metadata.gameName != impl.manifest.name)
        return fail("the save is for '" + data->metadata.gameName + "', not '" + impl.manifest.name + "'");
    if (data->metadata.scenePath.empty()) return fail("the save does not name a scene");

    PlayerSession fresh;
    if (!impl.build_session(fresh, data->metadata.scenePath, false, &stepError)) return fail(stepError);
    if (const auto saved = data->metadata.info.find("physics");
        saved != data->metadata.info.end() && saved->second != fresh.physicsBackend)
        impl.log("save was made with " + saved->second + " physics; continuing with " + fresh.physicsBackend);
    GameWorldRestoreOptions restoreOptions;
    restoreOptions.keepUnboundTimers = true;   // named Lua timers are bound again by load_state
    if (!fresh.world->restore_save_state(data->world, &result.restore, &stepError, restoreOptions)) return fail(stepError);
    result.worldStateHash = fresh.world->state_hash();
    if (result.worldStateHash != data->metadata.worldStateHash)
        return fail("restored world does not match the save (state hash differs)");
    if (!restore_game_runtime_state(*fresh.world, data->runtimes, &result.runtimes, &stepError)) return fail(stepError);
    for (const std::string& warning : result.runtimes.warnings) impl.log("save: " + warning);
#if defined(DVE_HAVE_LUA)
    if (fresh.script && data->scriptState) {
        if (!fresh.script->load_state(*data->scriptState, &stepError)) return fail("script state: " + stepError);
    }
#endif
    result.namedTimersRestored = result.restore.timersUnbound - fresh.world->unbound_timer_ids().size();
    result.timersDropped = result.restore.timersDropped + fresh.world->drop_unbound_timers();
    if (result.timersDropped != 0U)
        impl.log(std::to_string(result.timersDropped) + " saved timer(s) had no callback after boot and were dropped");

    // Swap in the new world; the old session (script host first) is destroyed here.
    { PlayerSession old = std::move(impl.session); }
    impl.session = std::move(fresh);
    impl.ticks = data->metadata.tickCount;
    impl.pending.clear();
    result.tickCount = impl.ticks;
    return result;
}

} // namespace dve::player
