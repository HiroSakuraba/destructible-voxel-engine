#pragma once

#include <memory>

#include "dve/editor_native_renderer.hpp"
#include "dve/platform/application_host.hpp"

namespace dve::editor {

// SDL_Renderer-backed implementation used to prove that the shared editor composition can
// run unchanged on Windows, Linux, and macOS. It is a portability bootstrap, not the final
// high-performance voxel viewport renderer.
class SdlEditorCanvas final : public IEditorCanvas {
public:
    explicit SdlEditorCanvas(platform::NativeWindowHandle window, std::string* error = nullptr);
    ~SdlEditorCanvas() override;

    SdlEditorCanvas(const SdlEditorCanvas&) = delete;
    SdlEditorCanvas& operator=(const SdlEditorCanvas&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    bool begin_frame(EditorColor clearColor, std::string* error = nullptr);
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

} // namespace dve::editor
