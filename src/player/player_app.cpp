#include "dve/player/player_app.hpp"

#include "dve/game_scene_loader.hpp"

#if defined(DVE_HAVE_LUA)
#include "dve/game_script.hpp"
#endif

#include <algorithm>
#include <cmath>
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

struct PlayerApp::Impl {
    // Declaration order is destruction order in reverse: the script host (which holds
    // listeners into the world and a pointer to the content) goes first, then the world,
    // then the content source.
    std::unique_ptr<ContentSource> content;
    GameManifest manifest;
    ui::GameSettings settings;
    PlayerInput input;
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
    bool quit{};
    std::uint64_t ticks{};
};

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

    // 3. Input bindings (bind.* entries, decision D6).
    auto bindings = InputBindingTable::parse(impl.manifest.inputBindings, &stepError);
    if (!bindings) return fail("game.dvegame: " + stepError);
    impl.input = PlayerInput(std::move(*bindings));

    // 4. World + physics: Jolt when compiled in, otherwise the reference solver.
    Physics3DBackend resolved = Physics3DBackend::Reference;
    auto physics = create_physics3d_world(options.physicsBackend, &resolved, &stepError);
    if (!physics) return fail("physics: " + stepError);
    impl.physicsBackend = std::string(physics3d_backend_label(resolved));
    impl.world = std::make_unique<GameWorld>(std::move(physics));
    log("physics: " + impl.physicsBackend);

    // 5. Scene.
    impl.scenePath = options.sceneOverride.value_or(impl.manifest.entryScene);
    const GameSceneLoadResult scene = load_scene_into_game_world(*impl.content, impl.scenePath, *impl.world);
    if (!scene) return fail("scene '" + impl.scenePath + "': " + scene.error.message);
    impl.sceneName = scene.sceneName;
    impl.sceneObjects = scene.objects.size();
    log("scene: " + impl.sceneName + " (" + std::to_string(impl.sceneObjects) + " objects)");

    // 6. Scripts (after the scene so the startup script can find scene objects).
    const std::string scriptPath = impl.manifest.startup_script_or_default();
    const bool scriptPresent = impl.content->exists(scriptPath);
#if defined(DVE_HAVE_LUA)
    if (options.enableScripts) {
        impl.script = std::make_unique<GameScriptHost>(*impl.world);
        impl.script->set_content_source(impl.content.get());
        impl.script->set_log_sink([callback = options.log](bool isError, std::string message) {
            if (callback) callback(std::string(isError ? "lua error: " : "lua: ") + message);
        });
        if (scriptPresent) {
            if (!impl.script->run_content_file(scriptPath, &stepError))
                return fail("script '" + scriptPath + "': " + stepError);
            impl.scriptLoaded = true;
            impl.scriptPath = scriptPath;
            log("script: " + scriptPath);
        } else if (impl.manifest.startupScript) {
            return fail("script '" + scriptPath + "' is missing");
        }
    } else {
        log("script: disabled");
    }
#else
    if (scriptPresent) log("script: " + scriptPath + " skipped (built without Lua)");
#endif
    return app;
}

const ContentSource& PlayerApp::content() const noexcept { return *impl_->content; }
const GameManifest& PlayerApp::manifest() const noexcept { return impl_->manifest; }
const ui::GameSettings& PlayerApp::settings() const noexcept { return impl_->settings; }
GameWorld& PlayerApp::world() noexcept { return *impl_->world; }
const GameWorld& PlayerApp::world() const noexcept { return *impl_->world; }
const PlayerInput& PlayerApp::input() const noexcept { return impl_->input; }
std::string_view PlayerApp::physics_backend() const noexcept { return impl_->physicsBackend; }
bool PlayerApp::scripting_compiled_in() noexcept {
#if defined(DVE_HAVE_LUA)
    return true;
#else
    return false;
#endif
}
bool PlayerApp::script_loaded() const noexcept { return impl_->scriptLoaded; }
const std::string& PlayerApp::script_path() const noexcept { return impl_->scriptPath; }
const std::string& PlayerApp::scene_path() const noexcept { return impl_->scenePath; }
const std::string& PlayerApp::scene_name() const noexcept { return impl_->sceneName; }
std::size_t PlayerApp::scene_object_count() const noexcept { return impl_->sceneObjects; }

void PlayerApp::handle_event(const platform::PlatformEvent& event) {
    if (event.type == platform::EventType::QuitRequested) impl_->quit = true;
    impl_->input.handle_event(event);
}

bool PlayerApp::quit_requested() const noexcept { return impl_->quit; }
void PlayerApp::request_quit() noexcept { impl_->quit = true; }

void PlayerApp::tick(float fixedDeltaSeconds) {
    impl_->input.apply(*impl_->world);
    impl_->world->tick(fixedDeltaSeconds);
    ++impl_->ticks;
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
    if (impl_->script) {
        if (const auto eye = impl_->script->global_vector(std::string(kCameraEyeGlobal)))
            camera.position = {eye->x, eye->y, eye->z};
        if (const auto target = impl_->script->global_vector(std::string(kCameraTargetGlobal)))
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
    if (impl_->script) return impl_->script->environment();
#endif
    return RenderEnvironment{};
}

PlayerRenderView PlayerApp::render_view(std::vector<GameRenderObject>& objects, float aspect) const {
    objects = impl_->world->render_objects();
    PlayerRenderView view;
    view.objects = objects;
    view.camera = camera(aspect);
    view.environment = environment();
    return view;
}

std::optional<double> PlayerApp::script_global(const std::string& key) const {
#if defined(DVE_HAVE_LUA)
    if (impl_->script) return impl_->script->global_number(key);
#else
    (void)key;
#endif
    return std::nullopt;
}

} // namespace dve::player
