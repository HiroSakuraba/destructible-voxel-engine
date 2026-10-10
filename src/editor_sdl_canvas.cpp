#include "dve/editor_sdl_canvas.hpp"
#include "dve/editor_text_encoding.hpp"

#include <SDL3/SDL.h>
#if DVE_HAVE_SDL_TTF
#include <SDL3_ttf/SDL_ttf.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>

#include "dve/editor_ui_zoom.hpp"

namespace dve::editor {
namespace {
void set_error(std::string* error, const char* message) {
    if (error != nullptr) *error = message != nullptr ? message : "SDL renderer failure";
}
void set_color(SDL_Renderer* renderer, EditorColor color) {
    const Uint8 red = static_cast<Uint8>((color >> 16U) & 0xFFU);
    const Uint8 green = static_cast<Uint8>((color >> 8U) & 0xFFU);
    const Uint8 blue = static_cast<Uint8>(color & 0xFFU);
    (void)SDL_SetRenderDrawColor(renderer, red, green, blue, SDL_ALPHA_OPAQUE);
}
SDL_FRect to_frect(const UiRect& rect) noexcept {
    return {static_cast<float>(rect.x), static_cast<float>(rect.y),
            static_cast<float>(rect.width), static_cast<float>(rect.height)};
}

#if DVE_HAVE_SDL_TTF
std::string find_editor_font() {
    if (const char* configured = std::getenv("DVE_EDITOR_FONT"); configured != nullptr && *configured != '\0') {
        std::error_code ec;
        if (std::filesystem::exists(configured, ec)) return configured;
    }
    static constexpr std::array<const char*, 8> kCandidates{
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf",
        "/System/Library/Fonts/Menlo.ttc",
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/cour.ttf"};
    for (const char* candidate : kCandidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) return candidate;
    }
    return {};
}
#endif
} // namespace

int sdl_debug_text_magnification(float renderScale) noexcept {
    // The 8x8 debug font is ~0.73x the editor's 11 px base text; match that size with a
    // whole-number magnification so glyphs stay pixel-exact rather than blurred.
    const float wanted = (static_cast<float>(kUiBaseTextPixels) / 8.0F) * (std::isfinite(renderScale) ? renderScale : 1.0F);
    return std::clamp(static_cast<int>(std::lround(wanted)), 1, 8);
}

struct SdlEditorCanvas::Impl {
    SDL_Renderer* renderer{};
    float scale{1.0F};
    int debugMagnification{1};
    mutable std::string scratch;
#if DVE_HAVE_SDL_TTF
    struct CachedText { SDL_Texture* texture{}; int width{}; int height{}; std::uint64_t lastFrame{}; };
    bool ttfInitialized{};
    std::string fontPath;
    TTF_Font* font{};              // font at the current pixel size
    int fontPixels{};
    int fontAscent{};
    std::unordered_map<int, TTF_Font*> fonts;  // one per pixel size (per zoom step), never per frame
    mutable std::unordered_map<std::string, CachedText> textCache;
    mutable std::unordered_map<std::string, int> widthCache;
    std::uint64_t frame{};

    void clear_text_cache() const {
        for (auto& [key, entry] : textCache) if (entry.texture) SDL_DestroyTexture(entry.texture);
        textCache.clear();
        widthCache.clear();
    }
    void select_font(int pixels) {
        if (!ttfInitialized || fontPath.empty() || pixels == fontPixels) return;
        clear_text_cache();
        fontPixels = pixels;
        TTF_Font*& slot = fonts[pixels];
        if (slot == nullptr) {
            slot = TTF_OpenFont(fontPath.c_str(), static_cast<float>(pixels));
            if (slot != nullptr) TTF_SetFontHinting(slot, TTF_HINTING_LIGHT);
        }
        font = slot;
        fontAscent = font != nullptr ? TTF_GetFontAscent(font) : 0;
    }
    void evict_stale() const {
        if (textCache.size() < 1024U) return;
        for (auto it = textCache.begin(); it != textCache.end();) {
            if (frame - it->second.lastFrame > 120U) {
                if (it->second.texture) SDL_DestroyTexture(it->second.texture);
                it = textCache.erase(it);
            } else ++it;
        }
        if (widthCache.size() > 8192U) widthCache.clear();
    }
#endif
};

SdlEditorCanvas::SdlEditorCanvas(platform::NativeWindowHandle window, std::string* error)
    : impl_(std::make_unique<Impl>()) {
    if (window.backend != platform::HostBackend::SDL3 || window.window == nullptr) {
        set_error(error, "SDL editor canvas requires an SDL3 native-window handle");
        return;
    }
    impl_->renderer = SDL_CreateRenderer(static_cast<SDL_Window*>(window.window), nullptr);
    if (impl_->renderer == nullptr) { set_error(error, SDL_GetError()); return; }
#if DVE_HAVE_SDL_TTF
    impl_->fontPath = find_editor_font();
    if (!impl_->fontPath.empty()) impl_->ttfInitialized = TTF_Init();
#endif
    set_ui_zoom(1.0F);
}

SdlEditorCanvas::~SdlEditorCanvas() {
    if (!impl_) return;
#if DVE_HAVE_SDL_TTF
    impl_->clear_text_cache();
    for (auto& [pixels, font] : impl_->fonts) if (font) TTF_CloseFont(font);
    impl_->fonts.clear();
    if (impl_->ttfInitialized) TTF_Quit();
#endif
    if (impl_->renderer != nullptr) SDL_DestroyRenderer(impl_->renderer);
}

bool SdlEditorCanvas::set_vsync(bool enabled, std::string* error) {
    if (!valid()) { set_error(error, "SDL editor canvas is not initialized"); return false; }
    if (!SDL_SetRenderVSync(impl_->renderer, enabled ? 1 : 0)) {
        set_error(error, SDL_GetError());
        return false;
    }
    return true;
}

bool SdlEditorCanvas::valid() const noexcept { return impl_ && impl_->renderer != nullptr; }

void SdlEditorCanvas::set_ui_zoom(float zoom, float pixelDensity) {
    if (!impl_) return;
    const float density = std::isfinite(pixelDensity) && pixelDensity > 0.0F ? pixelDensity : 1.0F;
    impl_->scale = snap_ui_zoom(zoom) * density;
    impl_->debugMagnification = sdl_debug_text_magnification(impl_->scale);
#if DVE_HAVE_SDL_TTF
    impl_->select_font(std::max(kUiBaseTextPixels,
        static_cast<int>(std::lround(static_cast<float>(kUiBaseTextPixels) * impl_->scale))));
#endif
}

float SdlEditorCanvas::render_scale() const noexcept { return impl_ ? impl_->scale : 1.0F; }

bool SdlEditorCanvas::scalable_text() const noexcept {
#if DVE_HAVE_SDL_TTF
    return impl_ && impl_->font != nullptr;
#else
    return false;
#endif
}

std::string_view SdlEditorCanvas::text_backend() const noexcept {
    return scalable_text() ? "sdl_ttf" : "debug_text";
}

bool SdlEditorCanvas::begin_frame(EditorColor clearColor, std::string* error) {
    if (!valid()) { set_error(error, "SDL editor canvas is not initialized"); return false; }
    set_color(impl_->renderer, clearColor);
    if (!SDL_RenderClear(impl_->renderer)) { set_error(error, SDL_GetError()); return false; }
    return true;
}

bool SdlEditorCanvas::save_screenshot_bmp(const std::filesystem::path& path, std::string* error) {
    if (!valid()) { set_error(error, "SDL editor canvas is not initialized"); return false; }
    SDL_Surface* surface = SDL_RenderReadPixels(impl_->renderer, nullptr);
    if (surface == nullptr) { set_error(error, SDL_GetError()); return false; }
    const bool saved = SDL_SaveBMP(surface, path.string().c_str());
    if (!saved) set_error(error, SDL_GetError());
    SDL_DestroySurface(surface);
    return saved;
}

bool SdlEditorCanvas::end_frame(std::string* error) {
    if (!valid()) { set_error(error, "SDL editor canvas is not initialized"); return false; }
    if (!SDL_RenderPresent(impl_->renderer)) { set_error(error, SDL_GetError()); return false; }
#if DVE_HAVE_SDL_TTF
    ++impl_->frame;
    impl_->evict_stale();
#endif
    return true;
}

void SdlEditorCanvas::fill(UiRect rect, EditorColor color) const {
    if (!valid() || rect.width <= 0 || rect.height <= 0) return;
    set_color(impl_->renderer, color);
    const SDL_FRect target = to_frect(ui_zoom_to_physical(rect, impl_->scale));
    (void)SDL_RenderFillRect(impl_->renderer, &target);
}

void SdlEditorCanvas::outline(UiRect rect, EditorColor color) const {
    if (!valid() || rect.width <= 0 || rect.height <= 0) return;
    set_color(impl_->renderer, color);
    const UiRect physical = ui_zoom_to_physical(rect, impl_->scale);
    const int stroke = ui_zoom_stroke(1.0F, std::floor(impl_->scale));
    for (int inset = 0; inset < stroke && inset * 2 < physical.width && inset * 2 < physical.height; ++inset) {
        const SDL_FRect target = to_frect({physical.x + inset, physical.y + inset,
                                           physical.width - inset * 2, physical.height - inset * 2});
        (void)SDL_RenderRect(impl_->renderer, &target);
    }
}

void SdlEditorCanvas::line(int x1, int y1, int x2, int y2, EditorColor color, int width) const {
    if (!valid()) return;
    set_color(impl_->renderer, color);
    const float fx1 = static_cast<float>(ui_zoom_to_physical(x1, impl_->scale));
    const float fy1 = static_cast<float>(ui_zoom_to_physical(y1, impl_->scale));
    const float fx2 = static_cast<float>(ui_zoom_to_physical(x2, impl_->scale));
    const float fy2 = static_cast<float>(ui_zoom_to_physical(y2, impl_->scale));
    const int thickness = ui_zoom_stroke(static_cast<float>(std::max(1, width)), impl_->scale);
    // Offset perpendicular to the dominant direction so thick lines stay solid at any angle.
    const bool mostlyHorizontal = std::fabs(fx2 - fx1) >= std::fabs(fy2 - fy1);
    const int first = -(thickness - 1) / 2;
    for (int offset = first; offset < first + thickness; ++offset) {
        const float o = static_cast<float>(offset);
        if (mostlyHorizontal) (void)SDL_RenderLine(impl_->renderer, fx1, fy1 + o, fx2, fy2 + o);
        else (void)SDL_RenderLine(impl_->renderer, fx1 + o, fy1, fx2 + o, fy2);
    }
}

void SdlEditorCanvas::text(int x, int y, std::string_view value, EditorColor color) const {
    if (!valid() || value.empty()) return;
    // y is the text baseline in logical pixels (same contract as the X11 host).
    const int px = ui_zoom_to_physical(x, impl_->scale);
    const int baseline = ui_zoom_to_physical(y, impl_->scale);
#if DVE_HAVE_SDL_TTF
    if (impl_->font != nullptr) {
        std::string& key = impl_->scratch;
        key.assign(value);
        key.push_back('\x1f');
        key.append(std::to_string(color));
        auto it = impl_->textCache.find(key);
        if (it == impl_->textCache.end()) {
            const SDL_Color fg{static_cast<Uint8>((color >> 16U) & 0xFFU), static_cast<Uint8>((color >> 8U) & 0xFFU),
                               static_cast<Uint8>(color & 0xFFU), SDL_ALPHA_OPAQUE};
            SDL_Surface* surface = TTF_RenderText_Blended(impl_->font, value.data(), value.size(), fg);
            Impl::CachedText entry;
            if (surface != nullptr) {
                entry.texture = SDL_CreateTextureFromSurface(impl_->renderer, surface);
                entry.width = surface->w;
                entry.height = surface->h;
                SDL_DestroySurface(surface);
            }
            it = impl_->textCache.emplace(key, entry).first;
        }
        it->second.lastFrame = impl_->frame;
        if (it->second.texture != nullptr) {
            const SDL_FRect target{static_cast<float>(px), static_cast<float>(baseline - impl_->fontAscent),
                                   static_cast<float>(it->second.width), static_cast<float>(it->second.height)};
            (void)SDL_RenderTexture(impl_->renderer, it->second.texture, nullptr, &target);
        }
        return;
    }
#endif
    set_color(impl_->renderer, color);
    impl_->scratch.assign(value);
    const int magnification = impl_->debugMagnification;
    const float glyph = static_cast<float>(SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * magnification);
    const float top = static_cast<float>(baseline) - glyph;
    const float mag = static_cast<float>(magnification);
    if (magnification != 1) (void)SDL_SetRenderScale(impl_->renderer, mag, mag);
    (void)SDL_RenderDebugText(impl_->renderer, static_cast<float>(px) / mag, top / mag, impl_->scratch.c_str());
    if (magnification != 1) (void)SDL_SetRenderScale(impl_->renderer, 1.0F, 1.0F);
}

std::string_view SdlEditorCanvas::ellipsis() const {
#if DVE_HAVE_SDL_TTF
    if (impl_ && impl_->font != nullptr) return "\u2026";
#endif
    return "...";
}

int SdlEditorCanvas::text_width(std::string_view value) const {
    // Reported in logical pixels so layout code that measures text keeps working unchanged.
    const float scale = impl_ ? impl_->scale : 1.0F;
#if DVE_HAVE_SDL_TTF
    if (impl_ && impl_->font != nullptr) {
        std::string key(value);
        if (auto it = impl_->widthCache.find(key); it != impl_->widthCache.end()) return it->second;
        int width = 0;
        int height = 0;
        if (!TTF_GetStringSize(impl_->font, value.data(), value.size(), &width, &height)) width = 0;
        const int logical = static_cast<int>(std::ceil(static_cast<float>(width) / scale));
        impl_->widthCache.emplace(std::move(key), logical);
        return logical;
    }
#endif
    const int magnification = impl_ ? impl_->debugMagnification : 1;
    // SDL_RenderDebugText decodes UTF-8 and draws one cell per code point, not per byte.
    const float physical = static_cast<float>(utf8_code_point_count(value)) *
                           static_cast<float>(SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * magnification);
    return static_cast<int>(std::ceil(physical / scale));
}

} // namespace dve::editor
