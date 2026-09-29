#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#if DVE_HAVE_XFT
#include <X11/Xft/Xft.h>
#endif

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
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "dve/editor_accessibility.hpp"
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_ui_zoom.hpp"

namespace {
using namespace dve;
using namespace dve::editor;

// UI zoom (accessibility text/UI scale, see dve/editor_ui_zoom.hpp). The controller lays out
// and hit-tests in *logical* pixels (physical / zoom); this canvas maps logical -> physical on
// output and renders text with a font rasterized at the zoomed pixel size, so glyphs stay crisp
// and every widget, gap, and hit rectangle scales by the same factor.

class X11FontCache {
public:
    X11FontCache(Display* display, int screen, XFontStruct* coreFont)
        : display_(display), screen_(screen), coreFont_(coreFont) {}
    ~X11FontCache() { release(); }
    // Must run before XCloseDisplay(); idempotent.
    void release() noexcept {
        clear_scaled_text();
        if (maskGc_ != nullptr) { XFreeGC(display_, maskGc_); maskGc_ = nullptr; }
        for (auto& [pixels, font] : coreFonts_) if (font && font != coreFont_) XFreeFont(display_, font);
        coreFonts_.clear();
#if DVE_HAVE_XFT
        for (auto& [pixels, font] : fonts_) if (font) XftFontClose(display_, font);
        for (auto& [key, color] : colors_)
            XftColorFree(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_), &color);
        fonts_.clear();
        colors_.clear();
#endif
    }
    X11FontCache(const X11FontCache&) = delete;
    X11FontCache& operator=(const X11FontCache&) = delete;
    [[nodiscard]] XFontStruct* core_font() const noexcept { return coreFont_; }
    // Fallback when Xft is unavailable: the largest standard misc-fixed bitmap face that fits
    // the zoomed text size (6x13 at 100%, up to 10x20 at 200%), loaded once per size.
    [[nodiscard]] XFontStruct* core_font(int pixels) {
        if (auto it = coreFonts_.find(pixels); it != coreFonts_.end()) return it->second;
        XFontStruct* chosen = nullptr;
        static constexpr std::array<std::pair<int, const char*>, 5> kFaces{{
            {20, "10x20"}, {18, "9x18"}, {15, "9x15"}, {14, "7x14"}, {13, "fixed"}}};
        for (const auto& [height, name] : kFaces) {
            if (height > std::max(13, pixels + 2)) continue;
            if (std::string_view(name) == "fixed") break;
            if ((chosen = XLoadQueryFont(display_, name)) != nullptr) break;
        }
        if (chosen == nullptr) chosen = coreFont_;
        coreFonts_.emplace(pixels, chosen);
        return chosen;
    }
    // Last-resort fallback (no Xft and no larger bitmap face installed, e.g. only 6x13):
    // rasterize the string once with the core font into a 1-bit mask, magnify it by an
    // integer factor, and cache the mask. Drawing is then a clipped XFillRectangle per string.
    struct ScaledText { Pixmap mask{}; int width{}; int height{}; int ascent{}; };
    [[nodiscard]] const ScaledText* scaled_text(Drawable reference, XFontStruct* font, std::string_view value,
                                                int magnification) {
        if (font == nullptr || value.empty() || magnification < 2) return nullptr;
        std::string key(value);
        key.push_back('\x1f');
        key.append(std::to_string(magnification));
        if (auto it = scaledText_.find(key); it != scaledText_.end()) return &it->second;
        if (scaledText_.size() > 4096U) clear_scaled_text();
        const int width = XTextWidth(font, value.data(), static_cast<int>(value.size()));
        const int height = font->ascent + font->descent;
        if (width <= 0 || height <= 0) return nullptr;
        Pixmap source = XCreatePixmap(display_, reference, static_cast<unsigned>(width), static_cast<unsigned>(height), 1U);
        if (maskGc_ == nullptr) maskGc_ = XCreateGC(display_, source, 0, nullptr);
        XSetForeground(display_, maskGc_, 0);
        XFillRectangle(display_, source, maskGc_, 0, 0, static_cast<unsigned>(width), static_cast<unsigned>(height));
        XSetForeground(display_, maskGc_, 1);
        XSetFont(display_, maskGc_, font->fid);
        XDrawString(display_, source, maskGc_, 0, font->ascent, value.data(), static_cast<int>(value.size()));
        XImage* glyphs = XGetImage(display_, source, 0, 0, static_cast<unsigned>(width), static_cast<unsigned>(height), 1UL, XYPixmap);
        XFreePixmap(display_, source);
        if (glyphs == nullptr) return nullptr;
        const int scaledWidth = width * magnification;
        const int scaledHeight = height * magnification;
        const int bytesPerLine = (scaledWidth + 7) / 8;
        char* data = static_cast<char*>(std::calloc(static_cast<std::size_t>(bytesPerLine) * static_cast<std::size_t>(scaledHeight), 1U));
        XImage* scaled = data ? XCreateImage(display_, DefaultVisual(display_, screen_), 1U, XYBitmap, 0, data,
                                             static_cast<unsigned>(scaledWidth), static_cast<unsigned>(scaledHeight), 8, bytesPerLine)
                              : nullptr;
        if (scaled == nullptr) { std::free(data); XDestroyImage(glyphs); return nullptr; }
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                if (XGetPixel(glyphs, x, y) != 0UL)
                    for (int dy = 0; dy < magnification; ++dy)
                        for (int dx = 0; dx < magnification; ++dx)
                            XPutPixel(scaled, x * magnification + dx, y * magnification + dy, 1UL);
        XDestroyImage(glyphs);
        Pixmap mask = XCreatePixmap(display_, reference, static_cast<unsigned>(scaledWidth), static_cast<unsigned>(scaledHeight), 1U);
        // XYBitmap images paint 1 bits with the GC foreground and 0 bits with its background.
        XSetForeground(display_, maskGc_, 1);
        XSetBackground(display_, maskGc_, 0);
        XPutImage(display_, mask, maskGc_, scaled, 0, 0, 0, 0, static_cast<unsigned>(scaledWidth), static_cast<unsigned>(scaledHeight));
        XDestroyImage(scaled);
        return &scaledText_.emplace(std::move(key), ScaledText{mask, scaledWidth, scaledHeight, font->ascent * magnification}).first->second;
    }
#if DVE_HAVE_XFT
    // Font opened once per pixel size (i.e. per zoom step), never per frame.
    [[nodiscard]] XftFont* font(int pixels) {
        pixels = std::max(kUiBaseTextPixels, pixels);
        if (auto it = fonts_.find(pixels); it != fonts_.end()) return it->second;
        const std::string pattern = "DejaVu Sans Mono,monospace:pixelsize=" + std::to_string(pixels);
        XftFont* opened = XftFontOpenName(display_, screen_, pattern.c_str());
        fonts_.emplace(pixels, opened);
        return opened;
    }
    [[nodiscard]] const XftColor* color(EditorColor value) {
        const auto key = static_cast<std::uint32_t>(value);
        if (auto it = colors_.find(key); it != colors_.end()) return &it->second;
        XRenderColor render{static_cast<unsigned short>(((key >> 16U) & 0xFFU) * 257U),
                            static_cast<unsigned short>(((key >> 8U) & 0xFFU) * 257U),
                            static_cast<unsigned short>((key & 0xFFU) * 257U), 0xFFFF};
        XftColor allocated{};
        if (!XftColorAllocValue(display_, DefaultVisual(display_, screen_), DefaultColormap(display_, screen_),
                                &render, &allocated)) return nullptr;
        return &colors_.emplace(key, allocated).first->second;
    }
#endif
private:
    Display* display_{};
    int screen_{};
    XFontStruct* coreFont_{};
    std::unordered_map<int, XFontStruct*> coreFonts_;
    std::unordered_map<std::string, ScaledText> scaledText_;
    GC maskGc_{};
    void clear_scaled_text() noexcept {
        for (auto& [key, entry] : scaledText_) if (entry.mask) XFreePixmap(display_, entry.mask);
        scaledText_.clear();
    }
#if DVE_HAVE_XFT
    std::unordered_map<int, XftFont*> fonts_;
    std::unordered_map<std::uint32_t, XftColor> colors_;
#endif
};

class X11EditorCanvas final : public IEditorCanvas {
public:
    X11EditorCanvas(Display* display, Drawable target, GC gc, X11FontCache& fonts, float zoom
#if DVE_HAVE_XFT
                    , XftDraw* xftDraw
#endif
                    )
        : display_(display), target_(target), gc_(gc), fonts_(fonts), zoom_(zoom)
        , coreFont_(fonts.core_font(ui_zoom_text_pixel_size(zoom)))
#if DVE_HAVE_XFT
        , xftDraw_(xftDraw), xftFont_(xftDraw ? fonts.font(ui_zoom_text_pixel_size(zoom)) : nullptr)
#endif
    {
        if (coreFont_ != nullptr) {
            XSetFont(display_, gc_, coreFont_->fid);
            // Integer magnification for the bitmap fallback when no face near the zoomed size exists.
            const int target = ui_zoom_text_pixel_size(zoom);
            const int ascent = std::max(1, coreFont_->ascent);
            coreMagnification_ = std::max(1, static_cast<int>(std::lround(static_cast<float>(target) / static_cast<float>(ascent + 1))));
        }
    }

    void fill(UiRect rect, EditorColor value) const override {
        const UiRect p = to_physical(rect);
        XSetForeground(display_, gc_, static_cast<unsigned long>(value));
        XFillRectangle(display_, target_, gc_, p.x, p.y,
                       static_cast<unsigned>(std::max(0, p.width)),
                       static_cast<unsigned>(std::max(0, p.height)));
    }
    void outline(UiRect rect, EditorColor value) const override {
        const UiRect p = to_physical(rect);
        XSetForeground(display_, gc_, static_cast<unsigned long>(value));
        XSetLineAttributes(display_, gc_, static_cast<unsigned>(stroke()), LineSolid, CapButt, JoinMiter);
        XDrawRectangle(display_, target_, gc_, p.x, p.y,
                       static_cast<unsigned>(std::max(0, p.width - stroke())),
                       static_cast<unsigned>(std::max(0, p.height - stroke())));
        XSetLineAttributes(display_, gc_, 1, LineSolid, CapButt, JoinMiter);
    }
    void line(int x1, int y1, int x2, int y2, EditorColor value, int width = 1) const override {
        XSetForeground(display_, gc_, static_cast<unsigned long>(value));
        XSetLineAttributes(display_, gc_, static_cast<unsigned>(ui_zoom_stroke(static_cast<float>(width), zoom_)),
                           LineSolid, CapRound, JoinRound);
        XDrawLine(display_, target_, gc_, px(x1), px(y1), px(x2), px(y2));
        XSetLineAttributes(display_, gc_, 1, LineSolid, CapButt, JoinMiter);
    }
    void text(int x, int y, std::string_view value, EditorColor color) const override {
#if DVE_HAVE_XFT
        if (xftFont_ != nullptr) {
            if (const XftColor* c = fonts_.color(color))
                XftDrawStringUtf8(xftDraw_, c, xftFont_, px(x), px(y),
                                  reinterpret_cast<const FcChar8*>(value.data()), static_cast<int>(value.size()));
            return;
        }
#endif
        XSetForeground(display_, gc_, static_cast<unsigned long>(color));
        if (coreMagnification_ > 1) {
            if (const auto* scaled = fonts_.scaled_text(target_, coreFont_, value, coreMagnification_)) {
                const int left = px(x);
                const int top = px(y) - scaled->ascent;
                XSetClipMask(display_, gc_, scaled->mask);
                XSetClipOrigin(display_, gc_, left, top);
                XFillRectangle(display_, target_, gc_, left, top, static_cast<unsigned>(scaled->width),
                               static_cast<unsigned>(scaled->height));
                XSetClipMask(display_, gc_, 0L);
                XSetClipOrigin(display_, gc_, 0, 0);
                return;
            }
        }
        XDrawString(display_, target_, gc_, px(x), px(y), value.data(), static_cast<int>(value.size()));
    }
    // Reported in logical pixels so layout code that measures text keeps working unchanged.
    [[nodiscard]] int text_width(std::string_view value) const override {
#if DVE_HAVE_XFT
        if (xftFont_ != nullptr) {
            XGlyphInfo extents{};
            XftTextExtentsUtf8(display_, xftFont_, reinterpret_cast<const FcChar8*>(value.data()),
                               static_cast<int>(value.size()), &extents);
            return static_cast<int>(std::ceil(static_cast<float>(extents.xOff) / zoom_));
        }
#endif
        if (coreFont_ == nullptr) return static_cast<int>(value.size()) * 7;
        return static_cast<int>(std::ceil(
            static_cast<float>(XTextWidth(coreFont_, value.data(), static_cast<int>(value.size())) * coreMagnification_) / zoom_));
    }

private:
    [[nodiscard]] int px(int logical) const noexcept { return ui_zoom_to_physical(logical, zoom_); }
    [[nodiscard]] int stroke() const noexcept { return ui_zoom_stroke(1.0F, std::floor(zoom_)); }
    [[nodiscard]] UiRect to_physical(UiRect rect) const noexcept { return ui_zoom_to_physical(rect, zoom_); }

    Display* display_{};
    Drawable target_{};
    GC gc_{};
    X11FontCache& fonts_;
    float zoom_{1.0F};
    XFontStruct* coreFont_{};
    int coreMagnification_{1};
#if DVE_HAVE_XFT
    XftDraw* xftDraw_{};
    XftFont* xftFont_{};
#endif
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
    std::optional<float> cliZoom;
    int initialWidth = 1280;
    int initialHeight = 800;
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
        else if (argument == "--ui-zoom" && index + 1 < argc) cliZoom = std::strtof(argv[++index], nullptr);
        else if (argument == "--window-size" && index + 1 < argc) {
            const std::string size = argv[++index];
            const auto x = size.find('x');
            if (x != std::string::npos) {
                initialWidth = std::max(640, std::atoi(size.substr(0, x).c_str()));
                initialHeight = std::max(480, std::atoi(size.substr(x + 1).c_str()));
            }
        }
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
        // User-scope settings (UI zoom and everything else in Settings > User) persist here.
        controller.configure_user_settings(std::filesystem::current_path() / ".dve" / "user" / "editor_settings.txt");
        if (cliZoom) {
            // Session-scope override: applies to this run only; a hotkey or Settings > Apply
            // replaces it and persists to the User layer.
            (void)controller.workspace().settings().set(SettingScope::Session, kUiZoomSettingId,
                                                        static_cast<double>(snap_ui_zoom(*cliZoom)));
            controller.workspace().synchronize_preferences_from_settings();
        }
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
        float zoom = kUiZoomMin;
        int logicalWidth = 0;
        int logicalHeight = 0;
        // Re-derive the effective zoom (requested setting, capped by window size) and re-run
        // layout only when the zoom or the logical size actually changes.
        const auto sync_zoom = [&](int physicalWidth, int physicalHeight) {
            controller.set_ui_zoom_window_limit(max_ui_zoom_for_window(physicalWidth, physicalHeight));
            zoom = controller.effective_ui_zoom();
            const int nextWidth = ui_zoom_logical_extent(physicalWidth, zoom);
            const int nextHeight = ui_zoom_logical_extent(physicalHeight, zoom);
            if (nextWidth == logicalWidth && nextHeight == logicalHeight) return;
            logicalWidth = nextWidth;
            logicalHeight = nextHeight;
            controller.resize(logicalWidth, logicalHeight);
        };
        const auto logical = [&](int physical) { return ui_zoom_to_logical(physical, zoom); };
        sync_zoom(initialWidth, initialHeight);
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
        Window window = XCreateSimpleWindow(display, RootWindow(display, screen), 40, 40,
                                            static_cast<unsigned>(initialWidth), static_cast<unsigned>(initialHeight), 0,
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
        X11FontCache fontCache{display, screen, font};
        int width = initialWidth;
        int height = initialHeight;
        Pixmap backBuffer = XCreatePixmap(display, window, static_cast<unsigned>(width), static_cast<unsigned>(height), static_cast<unsigned>(depth));
#if DVE_HAVE_XFT
        XftDraw* xftDraw = XftDrawCreate(display, backBuffer, visual, DefaultColormap(display, screen));
#endif
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
                            XFreePixmap(display, backBuffer);
                            backBuffer = XCreatePixmap(display, window, static_cast<unsigned>(width), static_cast<unsigned>(height), static_cast<unsigned>(depth));
#if DVE_HAVE_XFT
                            XftDrawChange(xftDraw, backBuffer);
#endif
                            sync_zoom(width, height);
                        }
                        break;
                    case MotionNotify:
                        controller.pointer_move(logical(event.xmotion.x), logical(event.xmotion.y),
                                                (event.xmotion.state & ShiftMask) ? 1U : 0U);
                        break;
                    case ButtonPress:
                        if (event.xbutton.button == Button4 || event.xbutton.button == Button5) {
                            std::uint32_t wheelModifiers = 0U;
                            if ((event.xbutton.state & ShiftMask) != 0U) wheelModifiers |= 1U;
                            if ((event.xbutton.state & ControlMask) != 0U) wheelModifiers |= 2U;
                            if ((event.xbutton.state & Mod1Mask) != 0U) wheelModifiers |= 4U;
                            controller.pointer_wheel(event.xbutton.button == Button4 ? 1.0F : -1.0F,
                                                     logical(event.xbutton.x), logical(event.xbutton.y), wheelModifiers);
                        } else {
                            controller.pointer_down(map_button(event.xbutton.button), logical(event.xbutton.x), logical(event.xbutton.y),
                                                    (event.xbutton.state & ShiftMask) ? 1U : 0U);
                        }
                        break;
                    case ButtonRelease:
                        controller.pointer_up(map_button(event.xbutton.button), logical(event.xbutton.x), logical(event.xbutton.y),
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
                        // Ctrl+= / Ctrl+- / Ctrl+0 (UI zoom) are handled inside key_down and
                        // update the shared `editor.ui_scale` setting; sync_zoom picks it up below.
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
            sync_zoom(width, height);
            {
                const std::string title = controller.workspace().document().name() +
                    (controller.workspace().document().dirty() ? " *" : "") + " - DVE Native Editor";
                XStoreName(display, window, title.c_str());
            }
            X11EditorCanvas painter{display, backBuffer, gc, fontCache, zoom
#if DVE_HAVE_XFT
                                    , xftDraw
#endif
                                    };
            render_native_editor(painter, controller, logicalWidth, logicalHeight);
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

#if DVE_HAVE_XFT
        XftDrawDestroy(xftDraw);
#endif
        fontCache.release();
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
