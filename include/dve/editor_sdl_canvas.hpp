#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "dve/editor_native_renderer.hpp"
#include "dve/platform/application_host.hpp"

namespace dve::editor {

// SDL_Renderer-backed implementation used to prove that the shared editor composition can
// run unchanged on Windows, Linux, and macOS. It is a portability bootstrap, not the final
// high-performance voxel viewport renderer.
//
// UI zoom: all drawing calls take *logical* editor pixels. set_ui_zoom() sets the
// logical -> physical factor (UI zoom times the window's pixel density). Text uses SDL3_ttf
// with a monospace TTF rasterized at the zoomed size when the canvas was built with
// DVE_HAVE_SDL_TTF and a font is found (DVE_EDITOR_FONT or common system paths); otherwise it
// falls back to SDL's 8x8 debug font scaled by an integer factor so it stays legible.
class SdlEditorCanvas final : public IEditorCanvas {
public:
    explicit SdlEditorCanvas(platform::NativeWindowHandle window, std::string* error = nullptr);
    ~SdlEditorCanvas() override;

    SdlEditorCanvas(const SdlEditorCanvas&) = delete;
    SdlEditorCanvas& operator=(const SdlEditorCanvas&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    // zoom = UI zoom (1.0-2.0); pixelDensity = drawable pixels per window coordinate (HiDPI).
    void set_ui_zoom(float zoom, float pixelDensity = 1.0F);
    [[nodiscard]] float render_scale() const noexcept;
    // True when text is rendered with a real scalable font (SDL3_ttf) rather than debug text.
    [[nodiscard]] bool scalable_text() const noexcept;
    [[nodiscard]] std::string_view text_backend() const noexcept;
    bool begin_frame(EditorColor clearColor, std::string* error = nullptr);
    // Reads back the current frame (call before end_frame) and writes a BMP file.
    bool save_screenshot_bmp(const std::filesystem::path& path, std::string* error = nullptr);
    bool end_frame(std::string* error = nullptr);

    void fill(UiRect rect, EditorColor color) const override;
    void outline(UiRect rect, EditorColor color) const override;
    void line(int x1, int y1, int x2, int y2, EditorColor color, int width = 1) const override;
    void text(int x, int y, std::string_view value, EditorColor color) const override;
    [[nodiscard]] int text_width(std::string_view value) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Integer magnification applied to SDL's 8x8 debug font for a given render scale when no
// scalable font is available (1x at 100%, 2x at 125-175%, 3x at 200%).
[[nodiscard]] int sdl_debug_text_magnification(float renderScale) noexcept;

} // namespace dve::editor
