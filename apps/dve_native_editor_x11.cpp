#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

// Xlib defines a global `None` macro that collides with strongly typed DVE enum members.
#ifdef None
#undef None
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dve/editor_accessibility.hpp"
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"

namespace {
using namespace dve;
using namespace dve::editor;

class X11EditorCanvas final : public IEditorCanvas {
public:
    X11EditorCanvas(Display* display, Drawable target, GC gc, XFontStruct* font)
        : display_(display), target_(target), gc_(gc), font_(font) {}

    void fill(UiRect rect, EditorColor value) const override {
        XSetForeground(display_, gc_, static_cast<unsigned long>(value));
        XFillRectangle(display_, target_, gc_, rect.x, rect.y,
                       static_cast<unsigned>(std::max(0, rect.width)),
                       static_cast<unsigned>(std::max(0, rect.height)));
    }
    void outline(UiRect rect, EditorColor value) const override {
        XSetForeground(display_, gc_, static_cast<unsigned long>(value));
        XDrawRectangle(display_, target_, gc_, rect.x, rect.y,
                       static_cast<unsigned>(std::max(0, rect.width - 1)),
                       static_cast<unsigned>(std::max(0, rect.height - 1)));
    }
    void line(int x1, int y1, int x2, int y2, EditorColor value, int width = 1) const override {
        XSetForeground(display_, gc_, static_cast<unsigned long>(value));
        XSetLineAttributes(display_, gc_, static_cast<unsigned>(std::max(1, width)),
                           LineSolid, CapRound, JoinRound);
        XDrawLine(display_, target_, gc_, x1, y1, x2, y2);
        XSetLineAttributes(display_, gc_, 1, LineSolid, CapButt, JoinMiter);
    }
    void text(int x, int y, std::string_view value, EditorColor color) const override {
        XSetForeground(display_, gc_, static_cast<unsigned long>(color));
        XDrawString(display_, target_, gc_, x, y, value.data(), static_cast<int>(value.size()));
    }
    [[nodiscard]] int text_width(std::string_view value) const override {
        if (font_ == nullptr) return static_cast<int>(value.size()) * 7;
        return XTextWidth(font_, value.data(), static_cast<int>(value.size()));
    }

private:
    Display* display_{};
    Drawable target_{};
    GC gc_{};
    XFontStruct* font_{};
};

int mask_shift(unsigned long mask) {
    int shift = 0;
    while (mask && (mask & 1UL) == 0UL) { mask >>= 1U; ++shift; }
    return shift;
}
int mask_bits(unsigned long mask) {
    int bits = 0;
    while (mask) { bits += static_cast<int>(mask & 1UL); mask >>= 1U; }
    return bits;
}
unsigned extract_channel(unsigned long pixel, unsigned long mask) {
    if (!mask) return 0;
    const int shift = mask_shift(mask);
    const int bits = mask_bits(mask);
    const unsigned long raw = (pixel & mask) >> static_cast<unsigned>(shift);
    const unsigned long maximum = bits >= static_cast<int>(sizeof(unsigned long) * 8) ? ~0UL : ((1UL << static_cast<unsigned>(bits)) - 1UL);
    return maximum ? static_cast<unsigned>((raw * 255UL + maximum / 2UL) / maximum) : 0U;
}

bool save_ppm(Display* display, Drawable source, Visual* visual, int width, int height,
              const std::filesystem::path& path, std::string& error) {
    XImage* image = XGetImage(display, source, 0, 0, static_cast<unsigned>(width), static_cast<unsigned>(height), AllPlanes, ZPixmap);
    if (!image) { error = "XGetImage failed"; return false; }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) { XDestroyImage(image); error = "could not open screenshot"; return false; }
    output << "P6\n" << width << ' ' << height << "\n255\n";
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const unsigned long pixel = XGetPixel(image, x, y);
            const unsigned char channels[3]{
                static_cast<unsigned char>(extract_channel(pixel, visual->red_mask)),
                static_cast<unsigned char>(extract_channel(pixel, visual->green_mask)),
                static_cast<unsigned char>(extract_channel(pixel, visual->blue_mask)),
            };
            output.write(reinterpret_cast<const char*>(channels), 3);
        }
    }
    XDestroyImage(image);
    if (!output) { error = "failed to write screenshot"; return false; }
    return true;
}

PointerButton map_button(unsigned button) {
    if (button == Button1) return PointerButton::Primary;
    if (button == Button2) return PointerButton::Auxiliary;
    if (button == Button3) return PointerButton::Secondary;
    if (button == 8U) return PointerButton::Extra1;
    return PointerButton::NoButton;
}

std::string key_name(KeySym symbol) {
    if (symbol == XK_Escape) return "escape";
    if (symbol == XK_Tab || symbol == XK_ISO_Left_Tab) return "tab";
    if (symbol == XK_F1) return "f1";
    if (symbol == XK_F5) return "f5";
    if (symbol == XK_F6) return "f6";
    if (symbol == XK_Delete) return "delete";
    if (symbol == XK_BackSpace) return "backspace";
    if (symbol == XK_Return || symbol == XK_KP_Enter) return "return";
    if (symbol == XK_space) return "space";
    if (symbol == XK_Left) return "left";
    if (symbol == XK_Right) return "right";
    if (symbol == XK_Up) return "up";
    if (symbol == XK_Down) return "down";
    if (symbol == XK_plus) return "+";
    if (symbol == XK_equal) return "equal";
    if (symbol == XK_minus) return "minus";
    if (symbol == XK_bracketleft || symbol == XK_braceleft) return "[";
    if (symbol == XK_bracketright || symbol == XK_braceright) return "]";
    if (symbol == XK_F2) return "f2";
    const char* value = XKeysymToString(symbol);
    return value ? std::string(value) : std::string{};
}

} // namespace

using namespace dve;
using namespace dve::editor;

int main(int argc, char** argv) {
    bool smoke = false;
    std::filesystem::path screenshot;
    std::filesystem::path accessibilityDump;
    std::filesystem::path scenePath;
    std::filesystem::path synthPresetPath;
    std::filesystem::path projectRoot = std::filesystem::current_path();
    std::filesystem::path liveMcpRuntimeDirectory;
    bool liveMcp = false;
    bool liveMcpReadOnly = false;
    std::string synthPage;
    std::vector<std::string> dispatchActions;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--smoke") smoke = true;
        else if (argument == "--screenshot" && index + 1 < argc) screenshot = argv[++index];
        else if (argument == "--scene" && index + 1 < argc) scenePath = argv[++index];
        else if (argument == "--project-root" && index + 1 < argc) projectRoot = argv[++index];
        else if (argument == "--live-mcp") liveMcp = true;
        else if (argument == "--live-mcp-read-only") { liveMcp = true; liveMcpReadOnly = true; }
        else if (argument == "--live-mcp-runtime-dir" && index + 1 < argc) liveMcpRuntimeDirectory = argv[++index];
        else if (argument == "--synth-preset" && index + 1 < argc) synthPresetPath = argv[++index];
        else if (argument == "--accessibility-dump" && index + 1 < argc) accessibilityDump = argv[++index];
        else if (argument == "--synth-page" && index + 1 < argc) synthPage = argv[++index];
        // Repeatable: runs each named action through dispatch_action(), in order, once at
        // startup before the first frame renders. Meant for headless verification (combine
        // with --screenshot to capture a specific menu/tool/panel state without needing real
        // input) rather than for end users.
        else if (argument == "--dispatch" && index + 1 < argc) dispatchActions.push_back(argv[++index]);
    }

    try {
        EditorDocument document = make_new_project_document();
        if (!scenePath.empty()) {
            std::string error;
            auto loaded = EditorDocument::load(scenePath, &error);
            if (!loaded) throw std::runtime_error(error);
            document = std::move(*loaded);
        }
        NativeEditorController controller{EditorWorkspace(std::move(document))};
        controller.configure_menu_state(std::filesystem::current_path() / ".dve" / "user" / "editor_menu_state.txt");
        controller.configure_ai_assistant(projectRoot);
        if (liveMcp) {
            ai::LiveEditorMcpHostOptions liveOptions;
            liveOptions.projectRoot = projectRoot;
            liveOptions.runtimeDirectory = liveMcpRuntimeDirectory;
            liveOptions.approvalPolicy = liveMcpReadOnly
                ? ai::AiApprovalPolicy::ReadOnlyOnly : ai::AiApprovalPolicy::AskForChanges;
            std::string liveError;
            if (!controller.start_live_mcp_host(std::move(liveOptions), &liveError))
                throw std::runtime_error("could not start live-editor MCP host: " + liveError);
            std::cerr << "DVE live-editor MCP descriptor: "
                      << controller.live_mcp_status().descriptorPath << '\n';
        }
        if (!synthPresetPath.empty()) {
            std::string presetError;
            auto preset = audio::SynthPreset::load(synthPresetPath, &presetError);
            if (!preset) throw std::runtime_error("could not load synthesizer preset: " + presetError);
            controller.synthesizer().set_preset(*preset);
        }
        for (const std::string& action : dispatchActions) (void)controller.dispatch_action(action);
        if (!synthPage.empty() && controller.synth_panel().open()) {
            const std::string key = synthPage == "oscillators" ? "1" : synthPage == "filter" ? "3" :
                                    synthPage == "modulation" ? "4" : synthPage == "performance" ? "5" :
                                    synthPage == "effects" ? "6" : synthPage == "presets" ? "7" :
                                    synthPage == "expression" ? "8" : "";
            if (!key.empty()) (void)controller.synth_panel().key_down(key, controller.synthesizer());
        }
        controller.resize(1280, 800);
        if (!accessibilityDump.empty()) {
            std::string accessibilityError;
            if (!save_accessibility_tree_json(accessibilityDump, build_editor_accessibility_tree(controller),
                                              &accessibilityError))
                throw std::runtime_error(accessibilityError);
        }

        Display* display = XOpenDisplay(nullptr);
        if (!display) throw std::runtime_error("could not open X11 display");
        const int screen = DefaultScreen(display);
        Visual* visual = DefaultVisual(display, screen);
        const int depth = DefaultDepth(display, screen);
        Window window = XCreateSimpleWindow(display, RootWindow(display, screen), 40, 40, 1280, 800, 0,
                                            BlackPixel(display, screen), BlackPixel(display, screen));
        XStoreName(display, window, "DVE Native Editor v2.22");
        XSelectInput(display, window, ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask |
                                      ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
        Atom wmDelete = XInternAtom(display, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display, window, &wmDelete, 1);
        XMapWindow(display, window);
        GC gc = XCreateGC(display, window, 0, nullptr);
        XFontStruct* font = XLoadQueryFont(display, "fixed");
        if (font) XSetFont(display, gc, font->fid);
        int width = 1280;
        int height = 800;
        Pixmap backBuffer = XCreatePixmap(display, window, static_cast<unsigned>(width), static_cast<unsigned>(height), static_cast<unsigned>(depth));

        bool running = true;
        bool screenshotWritten = false;
        int frames = 0;
        auto previous = std::chrono::steady_clock::now();
        while (running) {
            while (XPending(display)) {
                XEvent event{};
                XNextEvent(display, &event);
                switch (event.type) {
                    case ClientMessage:
                        if (static_cast<Atom>(event.xclient.data.l[0]) == wmDelete) controller.request_quit();
                        break;
                    case ConfigureNotify:
                        if (event.xconfigure.width != width || event.xconfigure.height != height) {
                            width = std::max(640, event.xconfigure.width);
                            height = std::max(480, event.xconfigure.height);
                            controller.resize(width, height);
                            XFreePixmap(display, backBuffer);
                            backBuffer = XCreatePixmap(display, window, static_cast<unsigned>(width), static_cast<unsigned>(height), static_cast<unsigned>(depth));
                        }
                        break;
                    case MotionNotify:
                        controller.pointer_move(event.xmotion.x, event.xmotion.y,
                                                (event.xmotion.state & ShiftMask) ? 1U : 0U);
                        break;
                    case ButtonPress:
                        if (event.xbutton.button == Button4 || event.xbutton.button == Button5) {
                            std::uint32_t wheelModifiers = 0U;
                            if ((event.xbutton.state & ShiftMask) != 0U) wheelModifiers |= 1U;
                            if ((event.xbutton.state & ControlMask) != 0U) wheelModifiers |= 2U;
                            if ((event.xbutton.state & Mod1Mask) != 0U) wheelModifiers |= 4U;
                            controller.pointer_wheel(event.xbutton.button == Button4 ? 1.0F : -1.0F,
                                                     event.xbutton.x, event.xbutton.y, wheelModifiers);
                        } else {
                            controller.pointer_down(map_button(event.xbutton.button), event.xbutton.x, event.xbutton.y,
                                                    (event.xbutton.state & ShiftMask) ? 1U : 0U);
                        }
                        break;
                    case ButtonRelease:
                        controller.pointer_up(map_button(event.xbutton.button), event.xbutton.x, event.xbutton.y,
                                              (event.xbutton.state & ShiftMask) ? 1U : 0U);
                        break;
                    case KeyRelease: {
                        KeySym symbol = XLookupKeysym(&event.xkey, 0);
                        controller.key_up(key_name(symbol),
                                          (event.xkey.state & ControlMask) != 0,
                                          (event.xkey.state & ShiftMask) != 0,
                                          (event.xkey.state & Mod1Mask) != 0);
                        break;
                    }
                    case KeyPress: {
                        char typedBuffer[8] = {};
                        KeySym symbol{};
                        const int typedLength = XLookupString(&event.xkey, typedBuffer, sizeof(typedBuffer) - 1, &symbol, nullptr);
                        controller.key_down(key_name(symbol),
                                            (event.xkey.state & ControlMask) != 0,
                                            (event.xkey.state & ShiftMask) != 0,
                                            (event.xkey.state & Mod1Mask) != 0);
                        // Separate from key_down's own (lowercased, shortcut-oriented) printable
                        // handling: this carries the shift/caps-aware actual character, needed
                        // so renaming an object or typing a filter string preserves case rather
                        // than forcing everything to lowercase the way command-palette search
                        // input does (where case does not matter).
                        if (typedLength == 1 && !(event.xkey.state & ControlMask) && !(event.xkey.state & Mod1Mask) &&
                            std::isprint(static_cast<unsigned char>(typedBuffer[0]))) {
                            controller.text_input(typedBuffer[0]);
                        }
                        if (symbol == XK_q && (event.xkey.state & ControlMask)) controller.request_quit();
                        break;
                    }
                    default: break;
                }
            }
            if (controller.quit_requested()) { running = false; break; }
            const auto now = std::chrono::steady_clock::now();
            const float elapsed = std::chrono::duration<float>(now - previous).count();
            previous = now;
            controller.update(elapsed);
            {
                const std::string title = controller.workspace().document().name() +
                    (controller.workspace().document().dirty() ? " *" : "") + " - DVE Native Editor";
                XStoreName(display, window, title.c_str());
            }
            X11EditorCanvas painter{display, backBuffer, gc, font};
            render_native_editor(painter, controller, width, height);
            XCopyArea(display, backBuffer, window, gc, 0, 0, static_cast<unsigned>(width), static_cast<unsigned>(height), 0, 0);
            XFlush(display);
            ++frames;
            if (smoke && frames == 2 && dispatchActions.empty()) {
                (void)controller.dispatch_action("physics.simulate");
                (void)controller.dispatch_action("physics.stop");
                (void)controller.dispatch_action("window.toggle_audio");
                (void)controller.audio_mixer().synthesizer().note_on(48U, 0.65F);
            }
            if (!screenshot.empty() && !screenshotWritten && frames >= 3) {
                std::string error;
                if (!save_ppm(display, backBuffer, visual, width, height, screenshot, error))
                    throw std::runtime_error(error);
                screenshotWritten = true;
            }
            if (smoke && frames >= 4) running = false;
            if (!XPending(display)) {
                struct timespec delay{0, 8'000'000};
                nanosleep(&delay, nullptr);
            }
        }

        XFreePixmap(display, backBuffer);
        if (font) XFreeFont(display, font);
        XFreeGC(display, gc);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        if (smoke) {
            std::cout << "dve_native_editor_x11: PASS\n"
                      << "objects=" << controller.workspace().document().objects().size() << '\n'
                      << "draw_items=" << controller.draw_items().size() << '\n';
        }
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_native_editor_x11: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
