// dve_player: runs a game built on the engine from a .dvepak (or a loose project folder)
// in an SDL3 window, rendered on the CPU by the reference tracer (see docs/PACKAGING.md).
//
//   dve_player [game.dvepak | project-dir] [options]      (no argument: pak next to the exe)
//
// This file is only glue: argument parsing, the SDL window/texture presenter and the
// optional audio device. Everything else is platform-neutral in dve_player_runtime.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(DVE_PLAYER_HAVE_AUDIO)
#include "dve/audio/mixer.hpp"
#include "dve/audio/sdl_synth_audio_device.hpp"
#endif
#include "dve/content_source.hpp"
#include "dve/platform/sdl_application_host.hpp"
#include "dve/player/frame_image.hpp"
#include "dve/player/player_app.hpp"
#include "dve/player/player_renderer.hpp"
#include "dve/version.hpp"

namespace {

using namespace dve;
using namespace dve::player;
using Clock = std::chrono::steady_clock;

constexpr const char* kUsage = R"(usage: dve_player [<game.dvepak> | <project-dir>] [options]

content (first match wins; default: <exe>.dvepak, game.dvepak, content/game.dvepak next to the exe)
  --pak <file>              mount a .dvepak
  --project <dir>           run a loose project folder (development)
  --scene <path>            content path of the scene (default: game.dvegame entryScene)
window / rendering
  --size WxH                window size (default: game_settings.txt width/height)
  --fullscreen              fullscreen window
  --render-size WxH         CPU render resolution (default 480x270 x settings renderScale)
  --render-scale f          render at window size x f instead of a fixed size
  --threads N               CPU render threads (0 = all cores, default)
  --renderer cpu            renderer backend (only cpu exists today)
  --quality fast|balanced|reference   CPU lighting cost (default fast: hard shadows,
                            ambient GI; reference = scene environment unchanged)
  --headless                no window at all (render + readback only)
  --no-audio                do not open an audio device
  --no-script               do not run the Lua startup script
  --physics auto|reference|jolt
deterministic runs (tests)
  --frames N                run exactly N fixed steps (one render each), then exit
  --fixed-dt s              fixed step in seconds (default 1/60)
  --hold <key>@<from>-<to>  hold a key during frames [from, to) (repeatable)
  --hash                    print framebuffer_fnv of the last frame
  --expect-hash h[,h...]    exit 4 unless the hash matches one of these ...
  --reference-image f.ppm   ... or the frame, box-downsampled to f's size, is within
  --tolerance-mean m        mean |delta| <= m (default 1.5) and at most
  --tolerance-pixels p      p (default 0.01) of pixels differ by more than 24
  --write-reference f.ppm --reference-factor k   write a downsampled reference
  --screenshot f.png|.ppm   write the last rendered frame
  --window-screenshot f.bmp write the last presented window contents (SDL renderer)
  --version
)";

struct KeyHold {
    std::string key;
    std::uint64_t from{};
    std::uint64_t to{};
};

struct Options {
    std::optional<std::filesystem::path> content;
    bool contentIsPak{};
    bool contentIsProject{};
    std::optional<std::string> scene;
    std::optional<std::pair<int, int>> windowSize;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> renderSize;
    std::optional<float> renderScale;
    std::uint32_t threads{0};
    CpuRenderQuality quality{CpuRenderQuality::Fast};
    bool fullscreen{};
    bool headless{};
    bool noAudio{};
    bool noScript{};
    Physics3DBackend physics{Physics3DBackend::Automatic};
    std::optional<std::uint64_t> frames;
    float fixedDt{1.0F / 60.0F};
    std::vector<KeyHold> holds;
    bool hash{};
    std::vector<std::string> expectHashes;
    std::optional<std::filesystem::path> referenceImage;
    double toleranceMean{1.5};
    double tolerancePixels{0.01};
    std::optional<std::filesystem::path> writeReference;
    std::uint32_t referenceFactor{4};
    std::optional<std::filesystem::path> screenshot;
    std::optional<std::filesystem::path> windowScreenshot;
};

bool parse_size(const std::string& text, std::uint32_t& width, std::uint32_t& height) {
    const auto x = text.find('x');
    if (x == std::string::npos) return false;
    char* end = nullptr;
    const unsigned long w = std::strtoul(text.c_str(), &end, 10);
    if (end != text.c_str() + x) return false;
    const unsigned long h = std::strtoul(text.c_str() + x + 1, &end, 10);
    if (*end != '\0' || w == 0 || h == 0 || w > 8192 || h > 8192) return false;
    width = static_cast<std::uint32_t>(w);
    height = static_cast<std::uint32_t>(h);
    return true;
}

std::optional<Options> parse_arguments(int argc, char** argv, int* exitCode) {
    Options options;
    const auto bad = [&](const std::string& message) -> std::optional<Options> {
        std::cerr << "dve_player: " << message << "\n" << kUsage;
        *exitCode = kExitUsage;
        return std::nullopt;
    };
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto value = [&]() -> std::optional<std::string> {
            if (index + 1 >= argc) return std::nullopt;
            return std::string(argv[++index]);
        };
        std::optional<std::string> v;
        if (argument == "--help" || argument == "-h") {
            std::cout << kUsage;
            *exitCode = kExitOk;
            return std::nullopt;
        } else if (argument == "--version") {
            std::cout << "dve_player " << DVE_VERSION_STRING;
            if (DVE_GIT_DESCRIBE[0] != '\0') std::cout << " (" << DVE_GIT_DESCRIBE << ')';
            std::cout << '\n';
            *exitCode = kExitOk;
            return std::nullopt;
        } else if (argument == "--pak") {
            if (!(v = value())) return bad("--pak needs a file");
            options.content = *v;
            options.contentIsPak = true;
        } else if (argument == "--project") {
            if (!(v = value())) return bad("--project needs a folder");
            options.content = *v;
            options.contentIsProject = true;
        } else if (argument == "--scene") {
            if (!(v = value())) return bad("--scene needs a content path");
            options.scene = *v;
        } else if (argument == "--size") {
            std::uint32_t w{}, h{};
            if (!(v = value()) || !parse_size(*v, w, h)) return bad("--size needs WxH");
            options.windowSize = std::pair{static_cast<int>(w), static_cast<int>(h)};
        } else if (argument == "--render-size") {
            std::uint32_t w{}, h{};
            if (!(v = value()) || !parse_size(*v, w, h)) return bad("--render-size needs WxH");
            options.renderSize = std::pair{w, h};
        } else if (argument == "--render-scale") {
            if (!(v = value())) return bad("--render-scale needs a number");
            const float scale = std::strtof(v->c_str(), nullptr);
            if (!(scale > 0.05F && scale <= 2.0F)) return bad("--render-scale must be in (0.05, 2]");
            options.renderScale = scale;
        } else if (argument == "--threads") {
            if (!(v = value())) return bad("--threads needs a number");
            options.threads = static_cast<std::uint32_t>(std::strtoul(v->c_str(), nullptr, 10));
        } else if (argument == "--quality") {
            if (!(v = value()) || !parse_cpu_render_quality(*v, options.quality))
                return bad("--quality must be fast, balanced or reference");
        } else if (argument == "--renderer") {
            if (!(v = value()) || *v != "cpu") return bad("--renderer: only 'cpu' is available in this build");
        } else if (argument == "--fullscreen") {
            options.fullscreen = true;
        } else if (argument == "--headless") {
            options.headless = true;
        } else if (argument == "--no-audio") {
            options.noAudio = true;
        } else if (argument == "--no-script") {
            options.noScript = true;
        } else if (argument == "--physics") {
            if (!(v = value())) return bad("--physics needs auto|reference|jolt");
            if (*v == "auto") options.physics = Physics3DBackend::Automatic;
            else if (*v == "reference") options.physics = Physics3DBackend::Reference;
            else if (*v == "jolt") options.physics = Physics3DBackend::Jolt;
            else return bad("--physics needs auto|reference|jolt");
        } else if (argument == "--frames") {
            if (!(v = value())) return bad("--frames needs a count");
            const unsigned long long frames = std::strtoull(v->c_str(), nullptr, 10);
            if (frames == 0 || frames > 1000000ULL) return bad("--frames must be 1..1000000");
            options.frames = frames;
        } else if (argument == "--fixed-dt") {
            if (!(v = value())) return bad("--fixed-dt needs seconds");
            options.fixedDt = std::strtof(v->c_str(), nullptr);
            if (!(options.fixedDt > 0.0F && options.fixedDt <= 0.25F)) return bad("--fixed-dt must be in (0, 0.25]");
        } else if (argument == "--hold") {
            if (!(v = value())) return bad("--hold needs key@from-to");
            const auto at = v->rfind('@');
            const auto dash = v->find('-', at == std::string::npos ? 0 : at);
            if (at == std::string::npos || at == 0 || dash == std::string::npos) return bad("--hold needs key@from-to");
            KeyHold hold;
            hold.key = v->substr(0, at);
            hold.from = std::strtoull(v->substr(at + 1, dash - at - 1).c_str(), nullptr, 10);
            hold.to = std::strtoull(v->substr(dash + 1).c_str(), nullptr, 10);
            if (hold.to <= hold.from) return bad("--hold range must be from < to");
            options.holds.push_back(std::move(hold));
        } else if (argument == "--hash") {
            options.hash = true;
        } else if (argument == "--expect-hash") {
            if (!(v = value())) return bad("--expect-hash needs hashes");
            std::stringstream list(*v);
            for (std::string item; std::getline(list, item, ',');) if (!item.empty()) options.expectHashes.push_back(item);
            options.hash = true;
        } else if (argument == "--reference-image") {
            if (!(v = value())) return bad("--reference-image needs a .ppm");
            options.referenceImage = *v;
        } else if (argument == "--tolerance-mean") {
            if (!(v = value())) return bad("--tolerance-mean needs a number");
            options.toleranceMean = std::strtod(v->c_str(), nullptr);
        } else if (argument == "--tolerance-pixels") {
            if (!(v = value())) return bad("--tolerance-pixels needs a fraction");
            options.tolerancePixels = std::strtod(v->c_str(), nullptr);
        } else if (argument == "--write-reference") {
            if (!(v = value())) return bad("--write-reference needs a .ppm");
            options.writeReference = *v;
        } else if (argument == "--reference-factor") {
            if (!(v = value())) return bad("--reference-factor needs a number");
            options.referenceFactor = static_cast<std::uint32_t>(std::strtoul(v->c_str(), nullptr, 10));
        } else if (argument == "--screenshot") {
            if (!(v = value())) return bad("--screenshot needs a file");
            options.screenshot = *v;
        } else if (argument == "--window-screenshot") {
            if (!(v = value())) return bad("--window-screenshot needs a .bmp");
            options.windowScreenshot = *v;
        } else if (!argument.empty() && argument[0] != '-' && !options.content) {
            options.content = argument;
        } else {
            return bad("unknown argument '" + argument + "'");
        }
    }
    if (options.headless && options.windowScreenshot) return bad("--window-screenshot needs a window");
    return options;
}

std::filesystem::path executable_directory(bool useSdl) {
    if (useSdl) {
        if (const char* base = SDL_GetBasePath()) return std::filesystem::path(base);
    }
    std::error_code error;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) return self.parent_path();
    return std::filesystem::current_path(error);
}

std::filesystem::path executable_stem(const char* argv0) {
    std::error_code error;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) return self.stem();
    return std::filesystem::path(argv0 != nullptr ? argv0 : "dve_player").stem();
}

// Streams CPU frames into the window: one streaming RGBA texture, nearest-neighbour scaled
// to the largest aspect-preserving rectangle (letterboxed).
class SdlTextureBlitter final : public IFrameBlitter {
public:
    explicit SdlTextureBlitter(SDL_Window* window) : window_(window) {
        renderer_ = SDL_CreateRenderer(window, nullptr);
        if (renderer_ != nullptr) (void)SDL_SetRenderVSync(renderer_, 0);
    }
    ~SdlTextureBlitter() override {
        if (texture_ != nullptr) SDL_DestroyTexture(texture_);
        if (renderer_ != nullptr) SDL_DestroyRenderer(renderer_);
    }
    SdlTextureBlitter(const SdlTextureBlitter&) = delete;
    SdlTextureBlitter& operator=(const SdlTextureBlitter&) = delete;

    [[nodiscard]] bool valid() const noexcept { return renderer_ != nullptr; }
    [[nodiscard]] std::string renderer_name() const {
        const char* name = renderer_ != nullptr ? SDL_GetRendererName(renderer_) : nullptr;
        return name != nullptr ? name : "none";
    }

    bool blit(const Rgba8Image& frame, std::string* error) override {
        if (!frame.valid()) {
            if (error) *error = "invalid frame";
            return false;
        }
        if (texture_ == nullptr || textureWidth_ != frame.width || textureHeight_ != frame.height) {
            if (texture_ != nullptr) SDL_DestroyTexture(texture_);
            texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                         static_cast<int>(frame.width), static_cast<int>(frame.height));
            if (texture_ == nullptr) return sdl_error(error);
            (void)SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST);
            textureWidth_ = frame.width;
            textureHeight_ = frame.height;
        }
        if (!SDL_UpdateTexture(texture_, nullptr, frame.pixels.data(), static_cast<int>(frame.width * 4U)))
            return sdl_error(error);
        int outputWidth = 0;
        int outputHeight = 0;
        if (!SDL_GetCurrentRenderOutputSize(renderer_, &outputWidth, &outputHeight)) return sdl_error(error);
        const float scale = std::min(static_cast<float>(outputWidth) / static_cast<float>(frame.width),
                                     static_cast<float>(outputHeight) / static_cast<float>(frame.height));
        SDL_FRect destination;
        destination.w = static_cast<float>(frame.width) * scale;
        destination.h = static_cast<float>(frame.height) * scale;
        destination.x = (static_cast<float>(outputWidth) - destination.w) * 0.5F;
        destination.y = (static_cast<float>(outputHeight) - destination.h) * 0.5F;
        (void)SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        (void)SDL_RenderClear(renderer_);
        if (!SDL_RenderTexture(renderer_, texture_, nullptr, &destination)) return sdl_error(error);
        if (!pendingScreenshot_.empty()) {
            SDL_Surface* surface = SDL_RenderReadPixels(renderer_, nullptr);
            screenshotSaved_ = surface != nullptr && SDL_SaveBMP(surface, pendingScreenshot_.string().c_str());
            if (surface != nullptr) SDL_DestroySurface(surface);
            if (!screenshotSaved_) screenshotError_ = SDL_GetError();
            pendingScreenshot_.clear();
        }
        if (!SDL_RenderPresent(renderer_)) return sdl_error(error);
        return true;
    }

    void request_screenshot(std::filesystem::path path) { pendingScreenshot_ = std::move(path); }
    [[nodiscard]] bool screenshot_saved() const noexcept { return screenshotSaved_; }
    [[nodiscard]] const std::string& screenshot_error() const noexcept { return screenshotError_; }

private:
    static bool sdl_error(std::string* error) {
        if (error) *error = SDL_GetError();
        return false;
    }

    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    SDL_Texture* texture_{};
    std::uint32_t textureWidth_{};
    std::uint32_t textureHeight_{};
    std::filesystem::path pendingScreenshot_;
    bool screenshotSaved_{};
    std::string screenshotError_;
};

struct FrameTimes {
    double total{};
    double minimum{1.0e30};
    double maximum{};
    std::uint64_t count{};
    void add(double value) {
        total += value;
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
        ++count;
    }
    [[nodiscard]] double average() const { return count > 0 ? total / static_cast<double>(count) : 0.0; }
};

std::string format_ms(double value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << value;
    return out.str();
}

std::pair<std::uint32_t, std::uint32_t> internal_size(const Options& options, const ui::GameSettings& settings,
                                                      int windowWidth, int windowHeight) {
    if (options.renderSize) return *options.renderSize;
    if (options.renderScale) {
        return {std::max(1U, static_cast<std::uint32_t>(static_cast<float>(windowWidth) * *options.renderScale)),
                std::max(1U, static_cast<std::uint32_t>(static_cast<float>(windowHeight) * *options.renderScale))};
    }
    // Decision D5: 480x270 internal (scaled by the game's renderScale), upscaled to the window.
    const float scale = std::clamp(settings.renderScale, 0.25F, 2.0F);
    return {std::max(16U, static_cast<std::uint32_t>(480.0F * scale)),
            std::max(9U, static_cast<std::uint32_t>(270.0F * scale))};
}

int run(int argc, char** argv) {
    int exitCode = kExitOk;
    const std::optional<Options> parsed = parse_arguments(argc, argv, &exitCode);
    if (!parsed) return exitCode;
    const Options& options = *parsed;
    const bool deterministic = options.frames.has_value();
    const auto log = [](std::string_view line) { std::cerr << "dve_player: " << line << '\n'; };

    // --- Content ---------------------------------------------------------------------------
    std::filesystem::path location;
    if (options.content) {
        location = *options.content;
    } else {
        const std::filesystem::path directory = executable_directory(!options.headless);
        const std::vector<std::filesystem::path> candidates{
            directory / (executable_stem(argc > 0 ? argv[0] : nullptr).string() + ".dvepak"),
            directory / "game.dvepak", directory / "content" / "game.dvepak"};
        for (const auto& candidate : candidates) {
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error)) {
                location = candidate;
                break;
            }
        }
        if (location.empty()) {
            std::string message = "no game found. Pass a .dvepak or project folder, or place one at:";
            for (const auto& candidate : candidates) message += "\n  " + candidate.string();
            std::cerr << "dve_player: " << message << '\n';
            if (!options.headless && !deterministic)
                (void)SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "dve_player", message.c_str(), nullptr);
            return kExitUsage;
        }
    }
    std::error_code statError;
    if (options.contentIsPak && std::filesystem::is_directory(location, statError)) {
        std::cerr << "dve_player: --pak expects a file, got a folder: " << location.string() << '\n';
        return kExitContentError;
    }
    if (options.contentIsProject && !std::filesystem::is_directory(location, statError)) {
        std::cerr << "dve_player: --project expects a folder: " << location.string() << '\n';
        return kExitContentError;
    }
    std::string error;
    std::unique_ptr<ContentSource> content = open_content_source(location, &error);
    if (!content) {
        std::cerr << "dve_player: cannot open game content '" << location.string() << "': " << error << '\n';
        return kExitContentError;
    }

    PlayerBootOptions bootOptions;
    bootOptions.sceneOverride = options.scene;
    bootOptions.enableScripts = !options.noScript;
    bootOptions.physicsBackend = options.physics;
    bootOptions.log = log;
    std::unique_ptr<PlayerApp> app = PlayerApp::boot(std::move(content), bootOptions, &error);
    if (!app) {
        std::cerr << "dve_player: failed to start game: " << error << '\n';
        return kExitContentError;
    }
    const ui::GameSettings& settings = app->settings();

    // --- Window, presenter, audio -----------------------------------------------------------
    platform::SdlApplicationHost host;
    std::unique_ptr<SdlTextureBlitter> blitter;
    int windowWidth = options.windowSize ? options.windowSize->first : static_cast<int>(settings.width);
    int windowHeight = options.windowSize ? options.windowSize->second : static_cast<int>(settings.height);
    std::string videoDriver = "none";
    std::string presenter = "none";
    if (!options.headless) {
        platform::WindowDesc window;
        window.title = app->manifest().name;
        window.width = windowWidth;
        window.height = windowHeight;
        window.resizable = true;
        window.highDpi = settings.highDpi;
        window.fullscreen = options.fullscreen || settings.displayMode != "windowed";
        window.initializeAudio = !options.noAudio;
        if (!host.create_window(window, &error)) {
            std::cerr << "dve_player: cannot create window: " << error << '\n';
            return kExitRuntimeError;
        }
        const char* driver = SDL_GetCurrentVideoDriver();
        videoDriver = driver != nullptr ? driver : "unknown";
        blitter = std::make_unique<SdlTextureBlitter>(static_cast<SDL_Window*>(host.native_window_handle().window));
        if (!blitter->valid()) {
            std::cerr << "dve_player: cannot create SDL renderer: " << SDL_GetError() << '\n';
            return kExitRuntimeError;
        }
        presenter = "sdl-texture(" + blitter->renderer_name() + ")";
        const platform::WindowMetrics metrics = host.window_metrics();
        if (metrics.logicalWidth > 0) {
            windowWidth = metrics.logicalWidth;
            windowHeight = metrics.logicalHeight;
        }
    }

    // Audio is initialised separately from video and is optional: no driver or no device means
    // the game runs silently (one log line), never a failure.
    std::string audioStatus = "none (disabled)";
#if defined(DVE_PLAYER_HAVE_AUDIO)
    std::unique_ptr<audio::AudioMixer> mixer;
    std::unique_ptr<audio::SdlSynthAudioDevice> audioDevice;
    if (!options.headless && !options.noAudio) {
        mixer = std::make_unique<audio::AudioMixer>();
        std::string audioError;
        audioDevice = std::make_unique<audio::SdlSynthAudioDevice>(*mixer, &audioError);
        if (audioDevice->valid()) {
            const char* driver = SDL_GetCurrentAudioDriver();
            audioStatus = driver != nullptr ? driver : "sdl";
        } else {
            if (audioError.empty()) audioError = host.audio_init_error();
            if (audioError.empty()) audioError = "no audio device";
            log("audio unavailable, continuing without sound: " + audioError);
            audioDevice.reset();
            audioStatus = "none (" + audioError + ")";
        }
    }
#else
    if (!options.headless && !options.noAudio) {
        log("audio unavailable, continuing without sound: built without the audio synth");
        audioStatus = "none (not built)";
    }
#endif

    // --- Renderer ---------------------------------------------------------------------------
    CpuPlayerRendererOptions rendererOptions;
    rendererOptions.threadCount = options.threads;
    rendererOptions.quality = options.quality;
    std::unique_ptr<IPlayerRenderer> renderer = make_cpu_player_renderer(blitter.get(), rendererOptions);
    auto [renderWidth, renderHeight] = internal_size(options, settings, windowWidth, windowHeight);
    if (!renderer->resize(renderWidth, renderHeight, &error)) {
        std::cerr << "dve_player: " << error << '\n';
        return kExitRuntimeError;
    }
    log("renderer: " + std::string(renderer->name()) + " " + std::to_string(renderWidth) + "x" +
        std::to_string(renderHeight) + " -> " + presenter);

    std::vector<GameRenderObject> objects;
    FrameTimes renderTimes;
    FrameTimes frameTimes;
    FrameTimes tickTimes;
    std::uint64_t framesRendered = 0;
    const auto render_frame = [&](bool captureWindow) -> bool {
        const auto frameStart = Clock::now();
        const float aspect = static_cast<float>(renderWidth) / static_cast<float>(renderHeight);
        const PlayerRenderView view = app->render_view(objects, aspect);
        if (!renderer->render(view, &error)) return false;
        if (captureWindow && blitter && options.windowScreenshot) blitter->request_screenshot(*options.windowScreenshot);
        if (!renderer->present(&error)) return false;
        const PlayerRenderStats stats = renderer->last_stats();
        renderTimes.add(stats.renderMilliseconds);
        frameTimes.add(std::chrono::duration<double, std::milli>(Clock::now() - frameStart).count());
        ++framesRendered;
        return true;
    };
    const auto poll = [&]() {
        platform::PlatformEvent event;
        while (host.has_window() && host.poll_event(event)) {
            app->handle_event(event);
            if (event.type == platform::EventType::KeyDown && event.key == "escape") app->request_quit();
            if (event.type == platform::EventType::WindowResized && options.renderScale) {
                const auto [w, h] = internal_size(options, settings, event.width, event.height);
                if (renderer->resize(w, h, &error)) {
                    renderWidth = w;
                    renderHeight = h;
                }
            }
        }
    };

    // --- Loop -------------------------------------------------------------------------------
    if (deterministic) {
        // Exactly N fixed steps at fixedDt, one render per step, no wall clock involved.
        for (std::uint64_t frame = 0; frame < *options.frames; ++frame) {
            poll();
            for (const KeyHold& hold : options.holds) {
                platform::PlatformEvent event;
                event.key = hold.key;
                if (frame == hold.from) event.type = platform::EventType::KeyDown;
                else if (frame == hold.to) event.type = platform::EventType::KeyUp;
                else continue;
                app->handle_event(event);
            }
            const auto tickStart = Clock::now();
            app->tick(options.fixedDt);
            tickTimes.add(std::chrono::duration<double, std::milli>(Clock::now() - tickStart).count());
            if (!render_frame(frame + 1 == *options.frames)) {
                std::cerr << "dve_player: render failed: " << error << '\n';
                return kExitRuntimeError;
            }
            if (app->quit_requested()) break;
        }
    } else {
        FixedStepClock clock;
        clock.fixedDeltaSeconds = options.fixedDt;
        const double minimumFrameSeconds = 1.0 / static_cast<double>(std::max(30U, settings.frameRateLimit));
        auto previous = Clock::now();
        auto lastReport = previous;
        while (!app->quit_requested()) {
            poll();
            const auto now = Clock::now();
            const double elapsed = std::chrono::duration<double>(now - previous).count();
            previous = now;
            const std::uint32_t steps = clock.advance(elapsed);
            for (std::uint32_t step = 0; step < steps; ++step) app->tick(clock.fixedDeltaSeconds);
            if (!render_frame(false)) {
                std::cerr << "dve_player: render failed: " << error << '\n';
                return kExitRuntimeError;
            }
            if (std::chrono::duration<double>(Clock::now() - lastReport).count() >= 5.0) {
                log("frame " + format_ms(frameTimes.average()) + " ms avg (render " +
                    format_ms(renderTimes.average()) + " ms)");
                lastReport = Clock::now();
            }
            const double spent = std::chrono::duration<double>(Clock::now() - now).count();
            if (spent < minimumFrameSeconds)
                std::this_thread::sleep_for(std::chrono::duration<double>(minimumFrameSeconds - spent));
        }
    }

    // --- Report -----------------------------------------------------------------------------
    const Rgba8Image* frame = renderer->readback();
    std::cout << "content=" << app->content().describe() << '\n'
              << "game=" << app->manifest().name << ' ' << app->manifest().version << '\n'
              << "scene=" << app->scene_path() << " (" << app->scene_name() << ")\n"
              << "objects=" << app->world().object_count() << '\n'
              << "renderer=" << renderer->name() << '\n'
              << "presenter=" << presenter << '\n'
              << "video=" << videoDriver << '\n'
              << "audio=" << audioStatus << '\n'
              << "physics=" << app->physics_backend() << '\n'
              << "lua=" << (PlayerApp::scripting_compiled_in() ? "on" : "off") << '\n'
              << "script=" << (app->script_loaded() ? app->script_path() : std::string("none")) << '\n'
              << "render_size=" << renderWidth << 'x' << renderHeight << '\n'
              << "quality=" << cpu_render_quality_name(options.quality) << '\n'
              << "threads=" << (options.threads == 0 ? std::string("auto") : std::to_string(options.threads)) << '\n'
              << "frames=" << framesRendered << '\n'
              << "ticks=" << app->tick_count() << '\n'
              << "render_ms_avg=" << format_ms(renderTimes.average()) << '\n'
              << "render_ms_min=" << format_ms(renderTimes.count ? renderTimes.minimum : 0.0) << '\n'
              << "render_ms_max=" << format_ms(renderTimes.maximum) << '\n'
              << "frame_ms_avg=" << format_ms(frameTimes.average()) << '\n'
              << "tick_ms_avg=" << format_ms(tickTimes.average()) << '\n';
    for (const GameRenderObject& object : app->world().render_objects()) {
        if (object.name == nullptr || object.name->empty()) continue;
        std::ostringstream position;
        position << std::fixed << std::setprecision(3) << object.transform.position.x << ','
                 << object.transform.position.y << ',' << object.transform.position.z;
        std::cout << "object." << *object.name << '=' << position.str() << '\n';
    }
    if (options.screenshot && frame) {
        const std::string extension = options.screenshot->extension().string();
        const bool ok = extension == ".ppm" ? write_ppm(*options.screenshot, *frame, &error)
                                            : write_png(*options.screenshot, *frame, &error);
        if (!ok) {
            std::cerr << "dve_player: screenshot failed: " << error << '\n';
            return kExitRuntimeError;
        }
        std::cout << "screenshot=" << options.screenshot->string() << '\n';
    }
    if (options.windowScreenshot) {
        if (blitter && blitter->screenshot_saved()) {
            std::cout << "window_screenshot=" << options.windowScreenshot->string() << '\n';
        } else {
            std::cerr << "dve_player: window screenshot failed: "
                      << (blitter ? blitter->screenshot_error() : std::string("no window")) << '\n';
            return kExitRuntimeError;
        }
    }
    if (options.writeReference && frame) {
        const auto reference = downsample_box(*frame, options.referenceFactor);
        if (!reference || !write_ppm(*options.writeReference, *reference, &error)) {
            std::cerr << "dve_player: cannot write reference (size must divide by the factor)\n";
            return kExitRuntimeError;
        }
        std::cout << "reference=" << options.writeReference->string() << '\n';
    }
    if (options.hash) {
        if (!frame) {
            std::cerr << "dve_player: no frame rendered\n";
            return kExitRuntimeError;
        }
        const std::string hash = format_image_hash(hash_image(*frame));
        std::cout << "framebuffer_fnv=" << hash << '\n';
        if (!options.expectHashes.empty() || options.referenceImage) {
            const bool exact = std::find(options.expectHashes.begin(), options.expectHashes.end(), hash) !=
                               options.expectHashes.end();
            if (exact) {
                std::cout << "hash_check=exact\n";
            } else if (options.referenceImage) {
                const auto reference = read_ppm(*options.referenceImage, &error);
                if (!reference) {
                    std::cerr << "dve_player: " << error << '\n';
                    return kExitHashMismatch;
                }
                const std::uint32_t factor = reference->width > 0 ? frame->width / reference->width : 0U;
                const auto small = factor > 0 ? downsample_box(*frame, factor) : std::nullopt;
                const auto difference = small ? compare_images(*small, *reference, 24U) : std::nullopt;
                if (!difference) {
                    std::cerr << "dve_player: reference image size does not match the frame\n";
                    return kExitHashMismatch;
                }
                const double overFraction = static_cast<double>(difference->pixelsOverThreshold) /
                                            static_cast<double>(difference->pixelCount);
                std::cout << "compare_mean_delta=" << format_ms(difference->meanAbsoluteDelta) << '\n'
                          << "compare_max_delta=" << difference->maxChannelDelta << '\n'
                          << "compare_pixels_over=" << difference->pixelsOverThreshold << '/'
                          << difference->pixelCount << '\n';
                if (difference->meanAbsoluteDelta <= options.toleranceMean && overFraction <= options.tolerancePixels) {
                    std::cout << "hash_check=tolerance\n";
                    std::cerr << "dve_player: hash " << hash << " not in the expected list; frame is within "
                              << "tolerance of the reference (add the hash for this compiler)\n";
                } else {
                    std::cout << "hash_check=mismatch\n";
                    return kExitHashMismatch;
                }
            } else {
                std::cout << "hash_check=mismatch\n";
                return kExitHashMismatch;
            }
        }
    }
    if (deterministic) std::cout << "dve_player: PASS\n";
    return kExitOk;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& exception) {
        std::cerr << "dve_player: fatal: " << exception.what() << '\n';
        return dve::player::kExitRuntimeError;
    }
}
