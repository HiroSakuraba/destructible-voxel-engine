#include "dve/editor_sdl_canvas.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <string>

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
}

struct SdlEditorCanvas::Impl { SDL_Renderer* renderer{}; };

SdlEditorCanvas::SdlEditorCanvas(platform::NativeWindowHandle window, std::string* error)
    : impl_(std::make_unique<Impl>()) {
    if (window.backend != platform::HostBackend::SDL3 || window.window == nullptr) {
        set_error(error, "SDL editor canvas requires an SDL3 native-window handle");
        return;
    }
    impl_->renderer = SDL_CreateRenderer(static_cast<SDL_Window*>(window.window), nullptr);
    if (impl_->renderer == nullptr) set_error(error, SDL_GetError());
}

SdlEditorCanvas::~SdlEditorCanvas() {
    if (impl_ && impl_->renderer != nullptr) SDL_DestroyRenderer(impl_->renderer);
}

bool SdlEditorCanvas::valid() const noexcept { return impl_ && impl_->renderer != nullptr; }

bool SdlEditorCanvas::begin_frame(EditorColor clearColor, std::string* error) {
    if (!valid()) { set_error(error, "SDL editor canvas is not initialized"); return false; }
    set_color(impl_->renderer, clearColor);
    if (!SDL_RenderClear(impl_->renderer)) { set_error(error, SDL_GetError()); return false; }
    return true;
}

bool SdlEditorCanvas::end_frame(std::string* error) {
    if (!valid()) { set_error(error, "SDL editor canvas is not initialized"); return false; }
    if (!SDL_RenderPresent(impl_->renderer)) { set_error(error, SDL_GetError()); return false; }
    return true;
}

void SdlEditorCanvas::fill(UiRect rect, EditorColor color) const {
    if (!valid() || rect.width <= 0 || rect.height <= 0) return;
    set_color(impl_->renderer, color);
    const SDL_FRect target{static_cast<float>(rect.x), static_cast<float>(rect.y),
                           static_cast<float>(rect.width), static_cast<float>(rect.height)};
    (void)SDL_RenderFillRect(impl_->renderer, &target);
}

void SdlEditorCanvas::outline(UiRect rect, EditorColor color) const {
    if (!valid() || rect.width <= 0 || rect.height <= 0) return;
    set_color(impl_->renderer, color);
    const SDL_FRect target{static_cast<float>(rect.x), static_cast<float>(rect.y),
                           static_cast<float>(rect.width), static_cast<float>(rect.height)};
    (void)SDL_RenderRect(impl_->renderer, &target);
}

void SdlEditorCanvas::line(int x1, int y1, int x2, int y2, EditorColor color, int width) const {
    if (!valid()) return;
    set_color(impl_->renderer, color);
    const int radius = std::max(0, width - 1) / 2;
    for (int offset = -radius; offset <= radius; ++offset) {
        (void)SDL_RenderLine(impl_->renderer, static_cast<float>(x1), static_cast<float>(y1 + offset),
                            static_cast<float>(x2), static_cast<float>(y2 + offset));
    }
}

void SdlEditorCanvas::text(int x, int y, std::string_view value, EditorColor color) const {
    if (!valid() || value.empty()) return;
    set_color(impl_->renderer, color);
    const std::string owned(value);
    (void)SDL_RenderDebugText(impl_->renderer, static_cast<float>(x),
                              static_cast<float>(y - SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE), owned.c_str());
}

int SdlEditorCanvas::text_width(std::string_view value) const {
    return static_cast<int>(value.size()) * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
}

} // namespace dve::editor
