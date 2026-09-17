#pragma once

#include <cstdint>
#include <string_view>

#include "dve/editor_native.hpp"

namespace dve::editor {

using EditorColor = std::uint32_t;

// Minimal immediate-mode drawing contract shared by the legacy X11 host and future SDL3,
// Win32, and Cocoa hosts. It deliberately contains no OS or graphics-API types.
class IEditorCanvas {
public:
    virtual ~IEditorCanvas() = default;
    virtual void fill(UiRect rect, EditorColor color) const = 0;
    virtual void outline(UiRect rect, EditorColor color) const = 0;
    virtual void line(int x1, int y1, int x2, int y2, EditorColor color, int width = 1) const = 0;
    virtual void text(int x, int y, std::string_view value, EditorColor color) const = 0;
    [[nodiscard]] virtual int text_width(std::string_view value) const = 0;
};

void render_native_editor(const IEditorCanvas& canvas, NativeEditorController& controller,
                          int width, int height);

} // namespace dve::editor
