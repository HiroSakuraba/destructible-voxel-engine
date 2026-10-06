#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
    // Marker used when text is elided. Canvases that cannot draw U+2026 (the
    // X11 core-font and SDL debug-text fallbacks) return "...".
    [[nodiscard]] virtual std::string_view ellipsis() const { return "\u2026"; }
};

// Longest prefix of `value` (on a UTF-8 boundary) plus canvas.ellipsis() whose
// width is <= maxWidth. Returns `value` unchanged when it already fits, and an
// empty string when not even the ellipsis fits.
[[nodiscard]] std::string elide_text_to_width(const IEditorCanvas& canvas, std::string_view value, int maxWidth);

// Glyph extent of editor text around its baseline, in logical pixels, for the 11 px UI font
// (kUiBaseTextPixels; DejaVu Sans Mono ink reaches about 8.5 px above and 2.6 px below the
// baseline). Canvases cannot clip glyphs, so clipping canvases use these to decide whether a
// line of text fits.
inline constexpr int kEditorTextAscent = 10;
inline constexpr int kEditorTextDescent = 3;

// Forwards to another canvas but keeps every primitive inside `clip`: fills are
// intersected with it, lines and the edges of partly visible outlines are cut at its
// border, and text is elided at its right edge (dropped when not even the ellipsis
// fits, or when its anchor lies left of the clip or its glyphs would reach past the
// top or bottom). The 3D viewport paints through one: its draw lists (voxel splats,
// grid, boxes, frustums, 3D-text and Gabor labels) keep items whose centre projects out
// to 1.2x the viewport (see project_with in editor_viewport.cpp), so without the clip
// they would land on the panels around it.
class RectClipCanvas final : public IEditorCanvas {
public:
    RectClipCanvas(const IEditorCanvas& inner, UiRect clip) : inner_(inner), clip_(clip) {}
    void fill(UiRect rect, EditorColor color) const override;
    void outline(UiRect rect, EditorColor color) const override;
    void line(int x1, int y1, int x2, int y2, EditorColor color, int width = 1) const override;
    // y is the text baseline.
    void text(int x, int y, std::string_view value, EditorColor color) const override;
    [[nodiscard]] int text_width(std::string_view value) const override { return inner_.text_width(value); }
    [[nodiscard]] std::string_view ellipsis() const override { return inner_.ellipsis(); }

private:
    const IEditorCanvas& inner_;
    UiRect clip_;
};

// One text draw that went through a CellFitCanvas.
struct FittedTextRecord {
    UiRect cell{};
    int x{};
    int y{};
    std::string drawn;
    std::string full;
    bool elided{};
};

// Forwards to another canvas, and fits every text draw into the first
// registered cell that contains the text origin (x, baseline - 4), eliding it
// with ellipsis(). Text outside every cell is fitted to `bounds`. When an
// elided cell is hovered (or registered as focused), draw_tooltip() shows the
// full text in a box next to the cell.
class CellFitCanvas final : public IEditorCanvas {
public:
    CellFitCanvas(const IEditorCanvas& inner, UiRect bounds, UiRect screen, int hoverX, int hoverY);
    // Earlier cells win. Empty cells are ignored.
    void add_cell(UiRect cell, bool focused = false);
    void fill(UiRect rect, EditorColor color) const override { inner_.fill(rect, color); }
    void outline(UiRect rect, EditorColor color) const override { inner_.outline(rect, color); }
    void line(int x1, int y1, int x2, int y2, EditorColor color, int width) const override {
        inner_.line(x1, y1, x2, y2, color, width);
    }
    void text(int x, int y, std::string_view value, EditorColor color) const override;
    [[nodiscard]] int text_width(std::string_view value) const override { return inner_.text_width(value); }
    [[nodiscard]] std::string_view ellipsis() const override { return inner_.ellipsis(); }
    // Draws the pending tooltip (hover wins over focus). Call after the panel.
    void draw_tooltip(EditorColor background, EditorColor border, EditorColor color) const;
    [[nodiscard]] const std::vector<FittedTextRecord>& records() const noexcept { return records_; }
    [[nodiscard]] std::optional<std::string> tooltip_text() const;

    // Test hook: when set, every CellFitCanvas also appends its records here.
    static void set_record_sink(std::vector<FittedTextRecord>* sink) noexcept;

private:
    struct Cell { UiRect rect; bool focused; };
    struct Tip { UiRect cell; std::string text; bool hovered; };
    const IEditorCanvas& inner_;
    UiRect bounds_;
    UiRect screen_;
    int hoverX_;
    int hoverY_;
    std::vector<Cell> cells_;
    mutable std::vector<FittedTextRecord> records_;
    mutable std::optional<Tip> tip_;
};

void render_native_editor(const IEditorCanvas& canvas, NativeEditorController& controller,
                          int width, int height);

} // namespace dve::editor
