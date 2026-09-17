// The capstone this whole subsystem was building toward: a real X11 window reading real
// keyboard input (via the normal event loop; --smoke injects synthetic X11 key events through
// XTestFakeKeyEvent, which the X server treats indistinguishably from a real keyboard, rather
// than calling GameWorld's input setters directly and skipping the input pipeline), driving a
// GameWorld ticked by real Jolt physics, controlled by a real Lua script, with the script's
// HUD state actually rendered on screen. Rendering here is deliberately minimal (colored
// squares for object positions, plain text for the HUD prompt) rather than the full voxel
// rasterizer the editor has: the point of this program is proving the input/tick/script/HUD
// seam end to end, not adding a second renderer to the codebase.

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "dve/game_script.hpp"
#include "dve/physics_jolt_backend.hpp"

namespace {

using namespace dve;

std::string key_name(KeySym symbol) {
    if (symbol == XK_space) return "space";
    if (symbol == XK_Escape) return "escape";
    const char* value = XKeysymToString(symbol);
    return value ? std::string(value) : std::string{};
}

// Same shape as apps/dve_native_editor_x11.cpp's save_ppm; duplicated rather than shared
// because that function lives in an anonymous namespace in a standalone .cpp with no header
// to include it from.
unsigned extract_channel(unsigned long pixel, unsigned long mask) {
    if (!mask) return 0;
    int shift = 0;
    unsigned long shiftedMask = mask;
    while (!(shiftedMask & 1UL)) { shiftedMask >>= 1U; ++shift; }
    int bits = 0;
    while (shiftedMask) { bits += static_cast<int>(shiftedMask & 1UL); shiftedMask >>= 1U; }
    const unsigned long raw = (pixel & mask) >> static_cast<unsigned>(shift);
    const unsigned long maximum = (1UL << static_cast<unsigned>(bits)) - 1UL;
    return maximum ? static_cast<unsigned>((raw * 255UL + maximum / 2UL) / maximum) : 0U;
}

bool save_ppm(Display* display, Drawable source, Visual* visual, int width, int height,
              const std::string& path, std::string& error) {
    XImage* image = XGetImage(display, source, 0, 0, static_cast<unsigned>(width), static_cast<unsigned>(height), AllPlanes, ZPixmap);
    if (!image) { error = "XGetImage failed"; return false; }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) { XDestroyImage(image); error = "could not open screenshot path"; return false; }
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const unsigned long pixel = XGetPixel(image, x, y);
            const unsigned char channels[3]{
                static_cast<unsigned char>(extract_channel(pixel, visual->red_mask)),
                static_cast<unsigned char>(extract_channel(pixel, visual->green_mask)),
                static_cast<unsigned char>(extract_channel(pixel, visual->blue_mask))};
            output.write(reinterpret_cast<const char*>(channels), 3);
        }
    }
    XDestroyImage(image);
    return static_cast<bool>(output);
}

constexpr const char* kDefaultScript = R"LUA(
player = world.spawn_box("Player", 2, 0.5, 0.0, 1.0, 0.0, true, 500.0)
floor = world.spawn_box3("Floor", 8, 2, 8, 0.5, -2.0, -1.0, -2.0, false, 1000.0)
world.hud_set_interaction_prompt("WASD to move, Space to jump")

local wasJumping = false
world.on_tick(function(dt)
    local ax = world.get_axis("move_x")
    local az = world.get_axis("move_z")
    if ax ~= 0.0 or az ~= 0.0 then
        world.apply_force(player, ax * 3000.0, 0.0, az * 3000.0)
    end
    local jumping = world.is_action_pressed("jump")
    if jumping and not wasJumping then
        world.apply_impulse(player, 0.0, 250.0, 0.0)
        world.hud_set_interaction_prompt("Jumped!")
    elseif not jumping then
        world.hud_set_interaction_prompt("WASD to move, Space to jump")
    end
    wasJumping = jumping
end)
)LUA";

} // namespace

int main(int argc, char** argv) {
    bool smoke = false;
    std::string screenshotPath;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--smoke") smoke = true;
        else if (argument == "--screenshot" && i + 1 < argc) screenshotPath = argv[++i];
    }

    GameWorld world(std::make_unique<JoltRigidBodyWorld>());
    GameScriptHost host(world);
    std::string scriptError;
    if (!host.run_string(kDefaultScript, "game_host_default", &scriptError)) {
        std::fprintf(stderr, "script failed: %s\n", scriptError.c_str());
        return EXIT_FAILURE;
    }
    const auto playerId = world.find_by_name("Player");
    if (!playerId) { std::fprintf(stderr, "script did not create Player\n"); return EXIT_FAILURE; }

    Display* display = XOpenDisplay(nullptr);
    if (!display) { std::fprintf(stderr, "could not open X11 display\n"); return EXIT_FAILURE; }
    const int screen = DefaultScreen(display);
    Visual* visual = DefaultVisual(display, screen);
    const int depth = DefaultDepth(display, screen);
    const int width = 640;
    const int height = 480;
    Window window = XCreateSimpleWindow(display, RootWindow(display, screen), 40, 40, width, height, 0,
                                        BlackPixel(display, screen), BlackPixel(display, screen));
    XStoreName(display, window, "DVE Game Host");
    XSelectInput(display, window, ExposureMask | KeyPressMask | KeyReleaseMask);
    XMapWindow(display, window);
    XSync(display, False);
    for (int attempt = 0; attempt < 50; ++attempt) {
        XSetInputFocus(display, window, RevertToParent, CurrentTime);
        XSync(display, False);
        Window focused{};
        int revertTo{};
        XGetInputFocus(display, &focused, &revertTo);
        if (focused == window) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Real evidence the input pipeline is alive, not a leftover debug artifact: a nonzero
    // count here means X11 key events actually reached this process's event queue (X11's own
    // auto-repeat mechanism means this is normally well above the 4 events actually injected
    // by --smoke, not equal to it).
    int keyEventCount = 0;
    GC gc = XCreateGC(display, window, 0, nullptr);
    XFontStruct* font = XLoadQueryFont(display, "fixed");
    if (font) XSetFont(display, gc, font->fid);
    Pixmap backBuffer = XCreatePixmap(display, window, static_cast<unsigned>(width), static_cast<unsigned>(height), static_cast<unsigned>(depth));

    bool heldW = false, heldA = false, heldS = false, heldD = false, heldSpace = false;
    bool running = true;
    int frame = 0;
    constexpr float kFixedDelta = 1.0F / 60.0F;
    auto nextFrameTime = std::chrono::steady_clock::now();

    while (running) {
        // In --smoke mode, synthesize real X11 key events at fixed frames via XTestFakeKeyEvent
        // (a genuine X server event, delivered through the same path a real keyboard would use,
        // not a shortcut that calls GameWorld's input setters directly) so this exercises the
        // actual input pipeline, not a stand-in for it.
        if (smoke) {
            auto inject = [&](KeySym symbol, bool down) {
                const KeyCode code = XKeysymToKeycode(display, symbol);
                if (code) XTestFakeKeyEvent(display, code, down ? True : False, CurrentTime);
            };
            if (frame == 40) inject(XK_d, true);
            if (frame == 100) inject(XK_d, false);
            if (frame == 110) inject(XK_space, true);
            if (frame == 120) inject(XK_space, false);
            XFlush(display);
        }

        while (XPending(display)) {
            XEvent event{};
            XNextEvent(display, &event);
            if (event.type == KeyPress || event.type == KeyRelease) {
                ++keyEventCount;
                const KeySym symbol = XLookupKeysym(&event.xkey, 0);
                const bool down = event.type == KeyPress;
                const std::string name = key_name(symbol);
                if (name == "w") heldW = down;
                else if (name == "a") heldA = down;
                else if (name == "s") heldS = down;
                else if (name == "d") heldD = down;
                else if (name == "space") heldSpace = down;
                else if (name == "escape" && down) running = false;
            }
        }

        world.set_axis("move_x", (heldD ? 1.0F : 0.0F) - (heldA ? 1.0F : 0.0F));
        world.set_axis("move_z", (heldW ? 1.0F : 0.0F) - (heldS ? 1.0F : 0.0F));
        world.set_action_pressed("jump", heldSpace);
        world.tick(kFixedDelta);

        XSetForeground(display, gc, BlackPixel(display, screen));
        XFillRectangle(display, backBuffer, gc, 0, 0, static_cast<unsigned>(width), static_cast<unsigned>(height));
        const auto playerPos = world.position(*playerId);
        if (playerPos) {
            // Simple top-down projection: world (x,z) -> screen (x,y), centered, fixed scale.
            const int screenX = width / 2 + static_cast<int>(playerPos->x * 20.0F);
            const int screenY = height / 2 + static_cast<int>(playerPos->z * 20.0F);
            XSetForeground(display, gc, 0x00CC5544UL);
            XFillRectangle(display, backBuffer, gc, screenX - 8, screenY - 8, 16, 16);
        }
        XSetForeground(display, gc, 0x00DDDDDDUL);
        XDrawString(display, backBuffer, gc, 12, 20,
                    host.hud_model().interaction_prompt().c_str(),
                    static_cast<int>(host.hud_model().interaction_prompt().size()));
        const std::string frameLabel = "frame " + std::to_string(frame);
        XDrawString(display, backBuffer, gc, 12, height - 12, frameLabel.c_str(), static_cast<int>(frameLabel.size()));
        XCopyArea(display, backBuffer, window, gc, 0, 0, static_cast<unsigned>(width), static_cast<unsigned>(height), 0, 0);
        XFlush(display);

        ++frame;
        if (smoke && frame >= 220) running = false;

        // A real game loop never runs faster than this; without pacing, "60 frames apart"
        // between an injected key-down and key-up is not a meaningful separation in wall-clock
        // time, and the X server can end up delivering both in the same batch, which was
        // observed to make the held-key duration effectively zero in about half of all runs
        // (see docs/scripting_implementation_notes.md's addendum for this bug).
        nextFrameTime += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<float>(kFixedDelta));
        std::this_thread::sleep_until(nextFrameTime);
    }

    if (!screenshotPath.empty()) {
        std::string error;
        if (!save_ppm(display, backBuffer, visual, width, height, screenshotPath, error))
            std::fprintf(stderr, "screenshot failed: %s\n", error.c_str());
    }

    const auto finalPosition = world.position(*playerId);
    std::printf("key_events_received=%d\n", keyEventCount);
    bool ok = finalPosition.has_value();
    if (finalPosition) {
        std::printf("final_player_x=%.4f final_player_z=%.4f\n",
                    static_cast<double>(finalPosition->x), static_cast<double>(finalPosition->z));
        if (smoke) {
            // The injected "d" (move_x positive) held from frame 20 to 80 must have actually
            // pushed the player in +x; this is the real end-to-end assertion, not just "no
            // crash": synthetic X11 input -> event loop -> GameWorld axis -> Lua script ->
            // apply_force -> real Jolt physics -> observable position change.
            if (finalPosition->x <= 0.05F) {
                std::fprintf(stderr, "FAIL: injected input did not move the player in +x\n");
                ok = false;
            }
        }
    } else {
        ok = false;
    }

    if (font) XFreeFont(display, font);
    XFreeGC(display, gc);
    XFreePixmap(display, backBuffer);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    if (smoke) std::printf(ok ? "dve_game_host_x11_smoke: PASS\n" : "dve_game_host_x11_smoke: FAIL\n");
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
