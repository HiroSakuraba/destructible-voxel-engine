// dve_player_runtime: frame images, input bindings, the fixed-step clock, and PlayerApp +
// the CPU renderer booting the sample game (tests/data/player_sample) headlessly from a
// loose folder and from a .dvepak; polygon (.dmesh) objects are drawn by the CPU renderer. Also a helper mode used by the CLI tests:
//   dve_player_runtime_tests --corrupt-entry <in.dvepak> <out.dvepak> <entry-path>
#include "dve/player/frame_image.hpp"
#include "dve/player/player_app.hpp"
#include "dve/player/player_input.hpp"
#include "dve/player/player_renderer.hpp"
#include "dve/geometry_build.hpp"
#include "dve/v235_foundations.hpp"

#include "support/export_scene_fixtures.hpp"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;
#define CHECK(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " #condition "\n"; ++failures; } } while (false)

using namespace dve;
using namespace dve::player;

const std::filesystem::path kSample = std::filesystem::path(DVE_TEST_SOURCE_DIR) / "tests" / "data" / "player_sample";

std::filesystem::path make_temp_dir(const char* tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
                      (std::string("dve_player_runtime_") + tag + "_" + std::to_string(stamp));
    std::filesystem::create_directories(path);
    return path;
}

std::filesystem::path pack_sample(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> inputs;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(kSample)) {
        if (entry.is_regular_file()) inputs.push_back(std::filesystem::relative(entry.path(), kSample));
    }
    const auto pak = directory / "sample.dvepak";
    std::string error;
    if (!build_dvepak(kSample, inputs, pak, {}, nullptr, &error)) {
        std::cerr << "pack failed: " << error << '\n';
        ++failures;
    }
    return pak;
}

bool corrupt_entry(const std::filesystem::path& input, const std::filesystem::path& output, const std::string& entryPath) {
    std::string error;
    const auto manifest = inspect_dvepak(input, &error);
    if (!manifest) {
        std::cerr << "inspect failed: " << error << '\n';
        return false;
    }
    const DvePakEntry* target = nullptr;
    for (const DvePakEntry& entry : manifest->entries) if (entry.path == entryPath) target = &entry;
    if (target == nullptr || target->size == 0U) {
        std::cerr << "no entry " << entryPath << '\n';
        return false;
    }
    std::error_code ec;
    std::filesystem::copy_file(input, output, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return false;
    std::fstream file(output, std::ios::binary | std::ios::in | std::ios::out);
    const auto offset = static_cast<std::streamoff>(target->offset + target->size / 2U);
    file.seekg(offset);
    char byte{};
    file.read(&byte, 1);
    byte = static_cast<char>(byte ^ 0x5A);
    file.seekp(offset);
    file.write(&byte, 1);
    return static_cast<bool>(file);
}

void test_frame_image() {
    Rgba8Image image;
    image.width = 4;
    image.height = 2;
    image.pixels.resize(4 * 2 * 4);
    for (std::size_t i = 0; i < image.pixels.size(); ++i) image.pixels[i] = static_cast<std::uint8_t>(i * 7U);
    CHECK(image.valid());
    const std::uint64_t hash = hash_image(image);
    CHECK(format_image_hash(hash).size() == 16U);
    Rgba8Image other = image;
    other.pixels[5] ^= 1U;
    CHECK(hash_image(other) != hash);
    Rgba8Image transposed = image;
    transposed.width = 2;
    transposed.height = 4;
    CHECK(hash_image(transposed) != hash); // dimensions are hashed too

    const auto dir = make_temp_dir("image");
    std::string error;
    CHECK(write_png(dir / "a.png", image, &error));
    {
        std::ifstream png(dir / "a.png", std::ios::binary);
        char signature[8]{};
        png.read(signature, 8);
        CHECK(std::string(signature + 1, 3) == "PNG");
    }
    CHECK(write_ppm(dir / "a.ppm", image, &error));
    const auto back = read_ppm(dir / "a.ppm", &error);
    CHECK(back && back->width == 4U && back->height == 2U);
    if (back) {
        for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
            CHECK(back->pixels[i] == image.pixels[i] && back->pixels[i + 2] == image.pixels[i + 2]);
            CHECK(back->pixels[i + 3] == 255U);
        }
    }
    const auto small = downsample_box(image, 2);
    CHECK(small && small->width == 2U && small->height == 1U);
    CHECK(!downsample_box(image, 3));
    const auto same = compare_images(image, image, 0);
    CHECK(same && same->maxChannelDelta == 0U && same->pixelsOverThreshold == 0U);
    const auto diff = compare_images(image, other, 0);
    CHECK(diff && diff->maxChannelDelta == 1U && diff->pixelsOverThreshold == 1U);
    CHECK(!compare_images(image, *small, 0));
    std::filesystem::remove_all(dir);
}

platform::PlatformEvent key(platform::EventType type, std::string name, bool repeat = false) {
    platform::PlatformEvent event;
    event.type = type;
    event.key = std::move(name);
    event.repeat = repeat;
    return event;
}

void test_input_bindings() {
    std::string error;
    const std::map<std::string, std::string> spec{
        {"move_x", "key:a/-1, key:d/+1, gamepad:leftx"},
        {"jump", "key:space,gamepad:south,mouse:left"},
        {"throttle", "gamepad:righttrigger/2"},
    };
    auto table = InputBindingTable::parse(spec, &error);
    CHECK(table.has_value());
    if (!table) {
        std::cerr << error << '\n';
        return;
    }
    CHECK(table->find("move_x") && table->find("move_x")->axis && table->find("move_x")->sources.size() == 3U);
    CHECK(table->find("jump") && !table->find("jump")->axis);
    CHECK(table->find("throttle") && table->find("throttle")->axis);

    PlayerInput input(*table);
    using platform::EventType;
    CHECK(input.axis("move_x") == 0.0F);
    input.handle_event(key(EventType::KeyDown, "d"));
    CHECK(input.axis("move_x") == 1.0F);
    input.handle_event(key(EventType::KeyDown, "a"));
    CHECK(input.axis("move_x") == 0.0F); // both held cancel out
    input.handle_event(key(EventType::KeyUp, "d"));
    CHECK(input.axis("move_x") == -1.0F);
    input.handle_event(key(EventType::KeyUp, "a"));

    platform::PlatformEvent stick;
    stick.type = EventType::GamepadAxisMotion;
    stick.gamepadAxis = platform::GamepadAxis::LeftX;
    stick.gamepadValue = 0.1F; // inside the dead zone
    input.handle_event(stick);
    CHECK(input.axis("move_x") == 0.0F);
    stick.gamepadValue = 0.6F;
    input.handle_event(stick);
    CHECK(std::abs(input.axis("move_x") - 0.6F) < 1.0e-6F);
    input.handle_event(key(EventType::KeyDown, "d"));
    CHECK(input.axis("move_x") == 1.0F); // clamped
    input.handle_event(key(EventType::KeyUp, "d"));

    CHECK(!input.action("jump"));
    input.handle_event(key(EventType::KeyDown, "Space"));
    CHECK(input.action("jump")); // key names are case-insensitive
    input.handle_event(key(EventType::KeyUp, "space"));
    platform::PlatformEvent button;
    button.type = EventType::GamepadButtonDown;
    button.gamepadButton = platform::GamepadButton::South;
    input.handle_event(button);
    CHECK(input.action("jump"));
    platform::PlatformEvent focus;
    focus.type = EventType::WindowFocusLost;
    input.handle_event(focus);
    CHECK(!input.action("jump") && input.axis("move_x") == 0.0F);

    GameWorld world(create_physics3d_world(Physics3DBackend::Reference));
    input.handle_event(key(EventType::KeyDown, "d"));
    input.apply(world);
    CHECK(world.get_axis("move_x") == 1.0F);
    CHECK(!world.is_action_pressed("jump"));

    for (const char* bad : {"key", "pedal:x", "gamepad:nope", "mouse:wheel", "key:a/abc", "key:/1", "key:a,"}) {
        const std::map<std::string, std::string> broken{{"bad", bad}};
        std::string message;
        CHECK(!InputBindingTable::parse(broken, &message));
        CHECK(message.find("bind.bad") == 0U);
    }
}

void test_fixed_step_clock() {
    FixedStepClock clock;
    CHECK(clock.advance(0.0) == 0U);
    const double step = static_cast<double>(clock.fixedDeltaSeconds);
    CHECK(clock.advance(step) == 1U);
    CHECK(clock.advance(step * 0.5) == 0U);
    CHECK(clock.advance(step * 0.5 + 1.0e-9) == 1U);
    CHECK(clock.advance(10.0) == 8U); // clamped to 0.25 s and max 8 steps, backlog dropped
    CHECK(clock.accumulator <= clock.fixedDeltaSeconds + 1.0e-6);
    CHECK(clock.advance(-1.0) == 0U);
}

struct Run {
    std::string hash;
    std::size_t objects{};
    std::string spinner;
};

Run run_sample(std::unique_ptr<ContentSource> content, std::uint32_t frames, std::uint32_t threads,
               std::string* bootError = nullptr, const std::vector<platform::PlatformEvent>& events = {}) {
    Run run;
    std::string error;
    PlayerBootOptions options;
    auto app = PlayerApp::boot(std::move(content), options, &error);
    if (!app) {
        if (bootError) *bootError = error;
        return run;
    }
    MemoryFrameBlitter blitter;
    CpuPlayerRendererOptions rendererOptions;
    rendererOptions.threadCount = threads;
    auto renderer = make_cpu_player_renderer(&blitter, rendererOptions);
    CHECK(renderer->name() == "cpu");
    CHECK(renderer->readback() == nullptr);
    CHECK(renderer->resize(96, 54, &error));
    for (const auto& event : events) app->handle_event(event);
    std::vector<GameRenderObject> objects;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        app->tick(1.0F / 60.0F);
        const auto view = app->render_view(objects, 96.0F / 54.0F);
        CHECK(renderer->render(view, &error));
        CHECK(renderer->present(&error));
    }
    CHECK(blitter.frames() == frames);
    const Rgba8Image* frame = renderer->readback();
    CHECK(frame != nullptr && frame->valid());
    if (frame) {
        run.hash = format_image_hash(hash_image(*frame));
        CHECK(hash_image(blitter.last()) == hash_image(*frame));
    }
    const auto stats = renderer->last_stats();
    CHECK(stats.voxelInstances == 4U); // the visual-only spinner is drawn too
    CHECK(stats.hitRays > 0U);
    run.objects = app->world().object_count();
    if (const auto id = app->world().find_by_name("Spinner")) {
        const auto position = app->world().position(*id);
        if (position) run.spinner = std::to_string(position->x);
        CHECK(app->world().has_collision(*id) == false);
    }
    return run;
}

void test_sample_boot_loose_and_pak() {
    const auto dir = make_temp_dir("sample");
    const auto pak = pack_sample(dir);
    std::string error;

    auto loose = LooseContentSource::open(kSample, &error);
    CHECK(loose != nullptr);
    {
        std::string bootError;
        auto app = PlayerApp::boot(LooseContentSource::open(kSample, &error), {}, &bootError);
        CHECK(app != nullptr);
        if (app) {
            CHECK(app->manifest().name == "Player Sample");
            CHECK(app->scene_object_count() == 4U);
            CHECK(app->settings().width == 960U);
            CHECK(!app->physics_backend().empty());
            CHECK(app->input().table().find("move_x") != nullptr);
            CHECK(app->script_loaded() == PlayerApp::scripting_compiled_in());
            const auto camera = app->camera(16.0F / 9.0F);
            CHECK(std::abs(camera.position.x - 3.3F) < 1.0e-6F && std::abs(camera.target.y - 0.9F) < 1.0e-6F);
            // 60 degrees horizontal at 16:9 is ~36 degrees vertical.
            CHECK(camera.verticalFieldOfViewRadians > 0.6F && camera.verticalFieldOfViewRadians < 0.65F);
        } else {
            std::cerr << bootError << '\n';
        }
    }

    const Run looseRun = run_sample(std::move(loose), 5, 2);
    const Run pakRun = run_sample(PakContentSource::open(pak, &error), 5, 2);
    const Run pakRunSingle = run_sample(PakContentSource::open(pak, &error), 5, 1);
    const Run pakRunMany = run_sample(PakContentSource::open(pak, &error), 5, 7);
    CHECK(!looseRun.hash.empty());
    CHECK(looseRun.hash == pakRun.hash);
    CHECK(pakRun.hash == pakRunSingle.hash);
    CHECK(pakRun.hash == pakRunMany.hash);
    CHECK(looseRun.objects == 4U && pakRun.objects == 4U);
    std::cout << "sample 96x54 after 5 frames: " << pakRun.hash << '\n';

    if (PlayerApp::scripting_compiled_in()) {
        // Lua (loaded from the pak, with `require` through the content) spins the spinner,
        // and bind.move_x=key:d/+1 moves it along +x.
        const Run moved = run_sample(PakContentSource::open(pak, &error), 5, 2, nullptr,
                                     {key(platform::EventType::KeyDown, "d")});
        CHECK(!moved.spinner.empty() && std::stof(moved.spinner) > std::stof(pakRun.spinner) + 0.05F);
        const Run later = run_sample(PakContentSource::open(pak, &error), 6, 2);
        CHECK(later.hash != pakRun.hash); // animated
    }

    // Corrupted entry: boot fails cleanly and names the integrity failure.
    const auto bad = dir / "bad.dvepak";
    CHECK(corrupt_entry(pak, bad, "scenes/ground.dvox"));
    std::string bootError;
    const Run corrupted = run_sample(PakContentSource::open(bad, &error), 1, 1, &bootError);
    CHECK(corrupted.hash.empty());
    CHECK(bootError.find("scene") != std::string::npos);
    CHECK(bootError.find("ntegrity") != std::string::npos || bootError.find("hash") != std::string::npos);
    std::cout << "corrupt pak: " << bootError << '\n';

    // Missing manifest and bad bindings fail with messages, never crash.
    const auto noManifest = dir / "empty_project";
    std::filesystem::create_directories(noManifest);
    bootError.clear();
    CHECK(!PlayerApp::boot(LooseContentSource::open(noManifest, &error), {}, &bootError));
    CHECK(bootError.find("game.dvegame") != std::string::npos);
    CHECK(!PlayerApp::boot(nullptr, {}, &bootError));

    const auto badBindings = dir / "bad_bindings";
    std::filesystem::copy(kSample, badBindings, std::filesystem::copy_options::recursive);
    {
        std::ofstream manifest(badBindings / "game.dvegame", std::ios::app);
        manifest << "bind.fire=joystick:trigger\n";
    }
    bootError.clear();
    CHECK(!PlayerApp::boot(LooseContentSource::open(badBindings, &error), {}, &bootError));
    CHECK(bootError.find("bind.fire") != std::string::npos);
    std::filesystem::remove_all(dir);
}

void test_scripts_disabled_and_polygon_seam() {
    std::string error;
    PlayerBootOptions options;
    options.enableScripts = false;
    auto app = PlayerApp::boot(LooseContentSource::open(kSample, &error), options, &error);
    CHECK(app != nullptr);
    if (!app) return;
    CHECK(!app->script_loaded());
    const auto spinner = app->world().find_by_name("Spinner");
    CHECK(spinner.has_value());
    const auto before = spinner ? app->world().position(*spinner) : std::nullopt;
    app->tick(1.0F / 60.0F);
    const auto after = spinner ? app->world().position(*spinner) : std::nullopt;
    CHECK(before && after && before->x == after->x); // nothing animates it without the script

    // The renderer accepts objects without a material table (default palette) and markers.
    GameObjectDesc box;
    box.name = "box";
    box.voxels = std::make_unique<VoxelObject>(77U);
    for (int x = 0; x < 3; ++x) box.voxels->set_voxel({x, 0, 0}, 5U);
    box.dynamic = false;
    box.transform.position = {0.0F, 0.3F, 1.0F};
    CHECK(app->world().create_object(std::move(box), &error) != kInvalidGameObjectId);
    GameObjectDesc marker;
    marker.name = "marker";
    CHECK(app->world().create_object(std::move(marker), &error) != kInvalidGameObjectId);
    auto renderer = make_cpu_player_renderer(nullptr);
    CHECK(!renderer->render({}, &error)); // no size yet
    CHECK(!renderer->resize(0, 10, &error));
    CHECK(renderer->resize(48, 27, &error));
    std::vector<GameRenderObject> objects;
    CHECK(renderer->render(app->render_view(objects, 48.0F / 27.0F), &error));
    CHECK(renderer->present(&error)); // null blitter: headless
    CHECK(renderer->last_stats().voxelInstances == 5U);
}

// Polygon objects must reach the framebuffer. The renderer rescales them to voxel units and
// used to keep the source content hash, so validation culled every polygon instance.
void test_cpu_renderer_draws_polygons() {
    std::string error;
    PlayerBootOptions options;
    options.enableScripts = false;
    auto app = PlayerApp::boot(LooseContentSource::open(kSample, &error), options, &error);
    CHECK(app != nullptr);
    if (!app) return;
    auto renderer = make_cpu_player_renderer(nullptr);
    CHECK(renderer->resize(96, 54, &error));
    std::vector<GameRenderObject> objects;
    CHECK(renderer->render(app->render_view(objects, 96.0F / 54.0F), &error));
    const Rgba8Image* before = renderer->readback();
    CHECK(before != nullptr);
    if (!before) return;
    const std::vector<std::uint8_t> baseline = before->pixels;
    CHECK(renderer->last_stats().polygonInstances == 0U);

    RigidTransform at;
    at.position = {0.0F, 1.2F, 0.0F}; // the sample camera looks at (0, 0.9, 0)
    const auto crate = app->world().spawn_visual_polygon_asset(
        test_fixtures::make_box_polygon({0.4F, 0.4F, 0.4F}, {0.9F, 0.1F, 0.1F, 1.0F}, 91U), "crate", at, &error);
    CHECK(crate != kInvalidGameObjectId);
    objects.clear();
    CHECK(renderer->render(app->render_view(objects, 96.0F / 54.0F), &error));
    CHECK(renderer->last_stats().polygonInstances == 1U);
    const Rgba8Image* after = renderer->readback();
    CHECK(after != nullptr && after->pixels.size() == baseline.size());
    if (!after || after->pixels.size() != baseline.size()) return;
    std::size_t reddish = 0;
    for (std::size_t i = 0; i + 3U < baseline.size(); i += 4U) {
        const bool changed = after->pixels[i] != baseline[i] || after->pixels[i + 1U] != baseline[i + 1U] ||
                             after->pixels[i + 2U] != baseline[i + 2U];
        if (changed && after->pixels[i] > after->pixels[i + 1U] + 40U && after->pixels[i] > after->pixels[i + 2U] + 40U) {
            ++reddish;
        }
    }
    CHECK(reddish > 50U); // the red box covers a visible patch of the 96x54 frame
}

// --- Save games ------------------------------------------------------------------------------

struct SaveRig {
    std::unique_ptr<PlayerApp> app;
    std::unique_ptr<IPlayerRenderer> renderer;
    MemoryFrameBlitter blitter;
    std::vector<GameRenderObject> objects;
};

std::unique_ptr<SaveRig> make_save_rig(const std::filesystem::path& saveDir, std::string* error,
                                       std::optional<std::string> loadSave = std::nullopt) {
    auto rig = std::make_unique<SaveRig>();
    PlayerBootOptions options;
    options.saveDirectory = saveDir;
    options.loadSave = std::move(loadSave);
    rig->app = PlayerApp::boot(LooseContentSource::open(kSample, error), options, error);
    if (!rig->app) return nullptr;
    CpuPlayerRendererOptions rendererOptions;
    rendererOptions.threadCount = 2;
    rig->renderer = make_cpu_player_renderer(&rig->blitter, rendererOptions);
    if (!rig->renderer->resize(96, 54, error)) return nullptr;
    return rig;
}

std::string render_hash(SaveRig& rig) {
    std::string error;
    const auto view = rig.app->render_view(rig.objects, 96.0F / 54.0F);
    CHECK(rig.renderer->render(view, &error));
    const Rgba8Image* frame = rig.renderer->readback();
    return frame ? format_image_hash(hash_image(*frame)) : std::string();
}

void press(PlayerApp& app, const char* name, int ticks = 1) {
    app.handle_event(key(platform::EventType::KeyDown, name));
    for (int i = 0; i < ticks; ++i) app.tick(1.0F / 60.0F);
    app.handle_event(key(platform::EventType::KeyUp, name));
}

void run_ticks(PlayerApp& app, int ticks) { for (int i = 0; i < ticks; ++i) app.tick(1.0F / 60.0F); }

void test_save_directories() {
    CHECK(save_directory_slug("Player Sample") == "player-sample");
    CHECK(save_directory_slug("  Über/Game: 2!  ") == "ber-game-2");
    CHECK(save_directory_slug("...") == "game");
    CHECK(save_directory_slug("a.b_c") == "a.b_c");
    CHECK(valid_save_slot_name("quicksave") && valid_save_slot_name("slot-1.v2") && valid_save_slot_name("A_b"));
    CHECK(!valid_save_slot_name("") && !valid_save_slot_name(".hidden") && !valid_save_slot_name("a/b") &&
          !valid_save_slot_name("a b") && !valid_save_slot_name("..") && !valid_save_slot_name(std::string(65, 'a')));
#if !defined(_WIN32) && !defined(__APPLE__)
    const char* oldXdg = std::getenv("XDG_DATA_HOME");
    const std::string savedXdg = oldXdg ? oldXdg : "";
    GameManifest manifest;
    manifest.name = "Player Sample";
    ::setenv("XDG_DATA_HOME", "/tmp/dve-xdg-test", 1);
    CHECK(user_data_directory() == std::filesystem::path("/tmp/dve-xdg-test"));
    CHECK(default_save_directory(manifest) == std::filesystem::path("/tmp/dve-xdg-test/dve/player-sample/saves"));
    ::setenv("XDG_DATA_HOME", "relative/ignored", 1);   // the XDG spec says to ignore relative paths
    if (const char* home = std::getenv("HOME"))
        CHECK(user_data_directory() == std::filesystem::path(home) / ".local" / "share");
    if (oldXdg) ::setenv("XDG_DATA_HOME", savedXdg.c_str(), 1);
    else ::unsetenv("XDG_DATA_HOME");
#endif
}

void test_save_load() {
    const auto dir = make_temp_dir("saves");
    std::string error;
    auto a = make_save_rig(dir, &error);
    CHECK(a != nullptr);
    if (!a) { std::cerr << error << '\n'; return; }
    CHECK(a->app->save_directory() == dir);
    CHECK(a->app->resolve_save("slot1") == dir / "slot1.dvesave");
    CHECK(a->app->resolve_save("other/x.dvesave") == std::filesystem::path("other/x.dvesave"));
    CHECK(!a->app->resolve_save("bad slot", &error));
    CHECK(a->app->input().table().find("quicksave") && a->app->input().table().find("quickload"));

    // Plain save -> load in a fresh app: identical world state and frame.
    run_ticks(*a->app, 12);
    const auto saved = a->app->save_game("plain", &error);
    CHECK(saved.has_value());
    if (!saved) { std::cerr << "save: " << error << '\n'; return; }
    CHECK(saved->stats.fileBytes == std::filesystem::file_size(dir / "plain.dvesave"));
    CHECK(saved->tickCount == 12U);
    const std::string savedFrame = render_hash(*a);
    {
        auto b = make_save_rig(dir, &error, std::string("plain"));
        CHECK(b != nullptr);
        if (b) {
            CHECK(b->app->tick_count() == 12U);
            CHECK(b->app->world().state_hash() == saved->worldStateHash);
            CHECK(b->app->world().state_hash() == a->app->world().state_hash());
            CHECK(render_hash(*b) == savedFrame);
        } else {
            std::cerr << "load: " << error << '\n';
        }
    }

    // Destruction (Lua blast, when scripts run), more ticks, then quicksave (F5) and a load.
    if (PlayerApp::scripting_compiled_in()) {
        const std::size_t before = a->app->world().object_count();
        press(*a->app, "b");
        CHECK(a->app->world().object_count() > before);   // the tower's top broke off
    }
    run_ticks(*a->app, 20);
    press(*a->app, "F5");
    CHECK(a->app->save_events().size() == 1U);
    if (a->app->save_events().size() == 1U) {
        const PlayerSaveEvent& event = a->app->save_events().front();
        CHECK(event.ok && !event.load && event.path == dir / "quicksave.dvesave" && event.fileBytes > 0U);
        std::cout << "sample quicksave after blast: " << event.fileBytes << " bytes\n";
    }
    const std::uint64_t quickHash = a->app->world().state_hash();
    const std::uint64_t quickTick = a->app->tick_count();
    const std::size_t quickObjects = a->app->world().object_count();
    const std::string quickFrame = render_hash(*a);
    const auto spinner = a->app->world().find_by_name("Spinner");
    const auto quickSpinner = spinner ? a->app->world().position(*spinner) : std::nullopt;

    // Keep playing, then quickload (F9) restores the quicksave in place.
    a->app->handle_event(key(platform::EventType::KeyDown, "d"));
    run_ticks(*a->app, 25);
    a->app->handle_event(key(platform::EventType::KeyUp, "d"));
    CHECK(a->app->world().state_hash() != quickHash);
    press(*a->app, "F9");
    CHECK(a->app->save_events().size() == 1U && a->app->save_events().front().ok && a->app->save_events().front().load);
    CHECK(a->app->world().state_hash() == quickHash);
    if (PlayerApp::scripting_compiled_in()) {
        // The blast's named "aftershock" timer was pending at the quicksave: it is bound again.
        const auto reload = a->app->load_game("quicksave", &error);
        CHECK(reload && reload->namedTimersRestored == 1U && reload->timersDropped == 0U);
        CHECK(reload && reload->runtimes.camerasRestored && reload->runtimes.gameplayRestored);
        CHECK(!a->app->script_global("aftershocks").has_value());
    }
    CHECK(a->app->tick_count() == quickTick);
    CHECK(a->app->world().object_count() == quickObjects);
    CHECK(render_hash(*a) == quickFrame);
    if (spinner && quickSpinner) {
        const auto now = a->app->world().position(*spinner);
        CHECK(now && now->x == quickSpinner->x);   // Lua state (on_save/on_load) came back too
    }

    // Continuing after a load is deterministic: a fresh process loading the quicksave and
    // the original both run 15 more ticks and end identical.
    auto c = make_save_rig(dir, &error, std::string("quicksave"));
    CHECK(c != nullptr);
    if (c) {
        CHECK(c->app->world().state_hash() == quickHash);
        CHECK(render_hash(*c) == quickFrame);
        run_ticks(*a->app, 15);
        run_ticks(*c->app, 15);
        CHECK(c->app->world().state_hash() == a->app->world().state_hash());
        CHECK(render_hash(*c) == render_hash(*a));
        if (PlayerApp::scripting_compiled_in()) {
            // 20 + 15 ticks after the blast: the restored aftershock fired in both processes,
            // and its environment change is identical.
            CHECK(a->app->script_global("aftershocks") == 1.0 && c->app->script_global("aftershocks") == 1.0);
            CHECK(a->app->environment().exposure == 1.25F && c->app->environment().exposure == 1.25F);
        }
    }

    // Lua-driven slots: key 1 saves "slot1" through world.save_game, key 2 loads it.
    if (PlayerApp::scripting_compiled_in()) {
        press(*a->app, "1");
        CHECK(a->app->save_events().size() == 1U && a->app->save_events().front().ok &&
              a->app->save_events().front().fromScript);
        CHECK(std::filesystem::exists(dir / "slot1.dvesave"));
        const std::uint64_t slotHash = a->app->world().state_hash();
        run_ticks(*a->app, 10);
        press(*a->app, "2");
        CHECK(a->app->save_events().size() == 1U && a->app->save_events().front().ok &&
              a->app->save_events().front().load);
        CHECK(a->app->world().state_hash() == slotHash);
    }

    // Corrupt and truncated saves fail cleanly and leave the running game untouched.
    const std::uint64_t liveHash = a->app->world().state_hash();
    const std::uint64_t liveTick = a->app->tick_count();
    const auto slot = dir / "plain.dvesave";
    std::vector<char> bytes(std::filesystem::file_size(slot));
    {
        std::ifstream in(slot, std::ios::binary);
        in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    {
        std::ofstream out(dir / "truncated.dvesave", std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size() / 2U));
    }
    {
        auto flipped = bytes;
        flipped[flipped.size() / 2U] = static_cast<char>(flipped[flipped.size() / 2U] ^ 0x40);
        std::ofstream out(dir / "flipped.dvesave", std::ios::binary);
        out.write(flipped.data(), static_cast<std::streamsize>(flipped.size()));
    }
    for (const char* bad : {"truncated", "flipped", "missing"}) {
        std::string message;
        CHECK(!a->app->load_game(bad, &message).has_value());
        CHECK(!message.empty());
        CHECK(a->app->world().state_hash() == liveHash && a->app->tick_count() == liveTick);
    }
    {
        std::string message;
        auto broken = make_save_rig(dir, &message, std::string("truncated"));
        CHECK(broken == nullptr);
        CHECK(!message.empty());
    }
    std::filesystem::remove_all(dir);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--corrupt-entry") {
        return corrupt_entry(argv[2], argv[3], argv[4]) ? 0 : 1;
    }
    if (argc == 5 && std::string(argv[1]) == "--truncate-file") {
        // --truncate-file <in> <out> <bytes>: copies the first <bytes> bytes (CLI save tests).
        std::ifstream in(argv[2], std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto keep = std::min<std::size_t>(bytes.size(), std::strtoull(argv[4], nullptr, 10));
        std::ofstream out(argv[3], std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(keep));
        return in.good() || in.eof() ? (out.good() ? 0 : 1) : 1;
    }
    if (!geometry_kind_supported(GeometryKind::Voxel)) {
        std::cout << "voxel geometry disabled in this profile; player sample skipped\n";
        test_frame_image();
        test_input_bindings();
        test_fixed_step_clock();
        return failures == 0 ? 0 : 1;
    }
    test_frame_image();
    test_input_bindings();
    test_fixed_step_clock();
    test_sample_boot_loose_and_pak();
    test_scripts_disabled_and_polygon_seam();
    test_cpu_renderer_draws_polygons();
    test_save_directories();
    test_save_load();
    if (failures == 0) std::cout << "dve_player_runtime_tests: PASS\n";
    return failures == 0 ? 0 : 1;
}
