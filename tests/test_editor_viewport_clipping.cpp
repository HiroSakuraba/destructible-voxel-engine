// The 3D viewport paints only inside its own rect.
//
// The editor draw lists keep items whose centre projects out to 1.2x the viewport (see
// project_with in editor_viewport.cpp), so voxel splats, bounding-box and frustum lines
// and grid lines near an edge reach past it. render_native_editor paints the viewport
// through a clipping canvas; without it those primitives landed on the outliner,
// inspector and bottom panel after a large orbit. The contract is visual: rasterizing
// a frame, every pixel outside the viewport must match a frame of the same editor
// state whose camera looks away from the scene.
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_viewport.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// Rasterizes fills, outlines and lines the way SdlEditorCanvas does at 100% zoom
// (thick lines are widened perpendicular to their dominant axis). Text is drawn as the
// solid box its glyphs can cover: 7 px per byte (text_width), from 9 px above the baseline
// to 2 px below it (the ink of the 11 px UI font).
class RasterCanvas final : public IEditorCanvas {
public:
    RasterCanvas(int width, int height)
        : width_(width), height_(height), pixels_(static_cast<std::size_t>(width * height), 0U) {}
    void fill(UiRect rect, EditorColor color) const override {
        if (rect.width <= 0 || rect.height <= 0) return;
        for (int y = std::max(0, rect.y); y < std::min(height_, rect.y + rect.height); ++y)
            for (int x = std::max(0, rect.x); x < std::min(width_, rect.x + rect.width); ++x)
                pixels_[static_cast<std::size_t>(y * width_ + x)] = color | 0xFF000000U;
    }
    void outline(UiRect rect, EditorColor color) const override {
        if (rect.width <= 0 || rect.height <= 0) return;
        fill({rect.x, rect.y, rect.width, 1}, color);
        fill({rect.x, rect.y + rect.height - 1, rect.width, 1}, color);
        fill({rect.x, rect.y, 1, rect.height}, color);
        fill({rect.x + rect.width - 1, rect.y, 1, rect.height}, color);
    }
    void line(int x1, int y1, int x2, int y2, EditorColor color, int width) const override {
        const int thickness = std::max(1, width);
        const bool mostlyHorizontal = std::abs(x2 - x1) >= std::abs(y2 - y1);
        const int first = -(thickness - 1) / 2;
        for (int offset = first; offset < first + thickness; ++offset) {
            if (mostlyHorizontal) segment(x1, y1 + offset, x2, y2 + offset, color);
            else segment(x1 + offset, y1, x2 + offset, y2, color);
        }
    }
    void text(int x, int y, std::string_view value, EditorColor color) const override {
        fill({x, y - 9, text_width(value), 12}, color);
    }
    [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
    [[nodiscard]] std::string_view ellipsis() const override { return "..."; }

    [[nodiscard]] std::uint32_t at(int x, int y) const { return pixels_[static_cast<std::size_t>(y * width_ + x)]; }

private:
    void plot(int x, int y, EditorColor color) const {
        if (x >= 0 && y >= 0 && x < width_ && y < height_)
            pixels_[static_cast<std::size_t>(y * width_ + x)] = color | 0xFF000000U;
    }
    void segment(int x1, int y1, int x2, int y2, EditorColor color) const {
        const int dx = std::abs(x2 - x1);
        const int dy = -std::abs(y2 - y1);
        const int sx = x1 < x2 ? 1 : -1;
        const int sy = y1 < y2 ? 1 : -1;
        int error = dx + dy;
        for (;;) {
            plot(x1, y1, color);
            if (x1 == x2 && y1 == y2) break;
            const int twice = 2 * error;
            if (twice >= dy) { error += dy; x1 += sx; }
            if (twice <= dx) { error += dx; y1 += sy; }
        }
    }
    int width_;
    int height_;
    mutable std::vector<std::uint32_t> pixels_;
};

constexpr int kWidth = 1280;
constexpr int kHeight = 800;

RasterCanvas render(NativeEditorController& controller) {
    RasterCanvas canvas(kWidth, kHeight);
    render_native_editor(canvas, controller, kWidth, kHeight);
    return canvas;
}

bool inside(UiRect rect, int x, int y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

// Number of pixels outside the viewport that differ from a frame whose camera looks
// the other way (nothing of the scene projects into it).
std::size_t pixels_leaked_outside_viewport(NativeEditorController& controller) {
    const RasterCanvas frame = render(controller);
    const EditorCamera saved = controller.camera();
    EditorCamera& camera = controller.camera();
    camera.target = {2.0F * saved.position.x - saved.target.x, 2.0F * saved.position.y - saved.target.y,
                     2.0F * saved.position.z - saved.target.z};
    const RasterCanvas reference = render(controller);
    controller.camera() = saved;
    (void)render(controller);

    const UiRect viewport = controller.layout().viewport;
    std::size_t leaked = 0;
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x)
            if (!inside(viewport, x, y) && frame.at(x, y) != reference.at(x, y)) ++leaked;
    return leaked;
}

// Whether some voxel splat of the current draw list crosses the viewport border, i.e.
// the frame would leak without clipping.
bool draw_list_reaches_past_viewport(NativeEditorController& controller) {
    (void)render(controller);
    const UiRect viewport = controller.layout().viewport;
    for (const EditorVoxelDrawItem& item : controller.draw_items()) {
        const int radius = std::max(1, static_cast<int>(item.pixelRadius));
        const int x = static_cast<int>(item.screenX);
        const int y = static_cast<int>(item.screenY);
        if (x - radius < viewport.x || y - radius < viewport.y || x + radius >= viewport.x + viewport.width ||
            y + radius >= viewport.y + viewport.height)
            return true;
    }
    return false;
}

void prepare(NativeEditorController& controller) {
    controller.resize(kWidth, kHeight);
    (void)render(controller);
}

// The reported repro: a large right-button orbit in a fresh project
// (dve_desktop_editor, 1280x800 window, drag from (620,400) by 20 x (8,2) px).
void test_large_orbit_stays_in_viewport() {
    NativeEditorController controller{EditorWorkspace(make_new_project_document())};
    prepare(controller);
    require(pixels_leaked_outside_viewport(controller) == 0U, "the startup view leaks outside the viewport");
    int x = 620;
    int y = 400;
    controller.pointer_move(x, y);
    controller.pointer_down(PointerButton::Secondary, x, y);
    for (int step = 0; step < 20; ++step) {
        x += 8;
        y += 2;
        controller.pointer_move(x, y);
    }
    controller.pointer_up(PointerButton::Secondary, x, y);
    require(draw_list_reaches_past_viewport(controller),
            "precondition: after the orbit some voxel splats should straddle the viewport border");
    const std::size_t leaked = pixels_leaked_outside_viewport(controller);
    require(leaked == 0U, "large orbit painted " + std::to_string(leaked) + " pixels outside the viewport");
    std::printf("large orbit: OK\n");
}

// Orbit sweep: the object is orbited, then panned onto each edge and corner of the
// viewport (perspective and orthographic); nothing may leak at any pose.
void test_orbit_sweep_stays_in_viewport() {
    NativeEditorController controller{EditorWorkspace(make_new_project_document())};
    prepare(controller);
    const EditorCamera start = controller.camera();
    const UiRect viewport = controller.layout().viewport;
    static constexpr int kEdges[8][2]{{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    constexpr int kPoses = 48;
    std::size_t straddling = 0;
    for (int pose = 0; pose < kPoses; ++pose) {
        EditorCamera& camera = controller.camera();
        camera = start;
        if (pose % 6 == 5) {
            camera.projection = EditorProjection::Orthographic;
            camera.orthographicHeight = 3.0F + static_cast<float>(pose % 4);
        }
        orbit_camera(camera, static_cast<float>(pose * 37 % 360), static_cast<float>(pose * 23 % 140 - 70), 0.006F);
        const int* edge = kEdges[pose % 8];
        const float jitter = static_cast<float>(pose % 3 - 1) * 24.0F;
        // pan_camera moves the view, so the object moves by the opposite amount on screen.
        pan_camera(camera, static_cast<float>(edge[0]) * (static_cast<float>(viewport.width) * 0.5F + jitter),
                   static_cast<float>(-edge[1]) * (static_cast<float>(viewport.height) * 0.5F + jitter), viewport);
        if (draw_list_reaches_past_viewport(controller)) ++straddling;
        const std::size_t leaked = pixels_leaked_outside_viewport(controller);
        require(leaked == 0U, "pose " + std::to_string(pose) + " painted " + std::to_string(leaked) +
                                  " pixels outside the viewport");
    }
    require(straddling >= kPoses / 2, "only " + std::to_string(straddling) +
                                          " poses put voxels across the viewport border; the sweep is not exercising clipping");
    std::printf("orbit sweep: OK (%zu of %d poses straddle the border)\n", straddling, kPoses);
}

// Records text draws (and nothing else) with the RasterCanvas metrics.
class TextRecorder final : public IEditorCanvas {
public:
    struct Draw { int x; int y; std::string value; };
    void fill(UiRect, EditorColor) const override {}
    void outline(UiRect, EditorColor) const override {}
    void line(int, int, int, int, EditorColor, int) const override {}
    void text(int x, int y, std::string_view value, EditorColor) const override { draws.push_back({x, y, std::string(value)}); }
    [[nodiscard]] int text_width(std::string_view value) const override { return static_cast<int>(value.size()) * 7; }
    [[nodiscard]] std::string_view ellipsis() const override { return "..."; }
    mutable std::vector<Draw> draws;
};

// Labels of objects partly off screen used to spill over the viewport edge: text was only
// dropped when its anchor lay outside, so a label starting inside ran past the right edge,
// and one whose baseline sat just inside the top drew its glyphs above it.
void test_text_stays_in_clip() {
    const UiRect clip{100, 50, 200, 100};  // x 100..299, y 50..149
    TextRecorder recorder;
    const RectClipCanvas canvas(recorder, clip);
    auto drawn = [&](int x, int y, std::string_view value) -> std::string {
        recorder.draws.clear();
        canvas.text(x, y, value, 0U);
        if (recorder.draws.empty()) return "<dropped>";
        const TextRecorder::Draw& draw = recorder.draws.front();
        require(draw.x == x && draw.y == y, "clipped text moved");
        require(draw.x + recorder.text_width(draw.value) <= clip.x + clip.width,
                "text '" + draw.value + "' runs past the right edge");
        require(draw.y - kEditorTextAscent >= clip.y && draw.y + kEditorTextDescent <= clip.y + clip.height,
                "text '" + draw.value + "' crosses the top or bottom");
        return draw.value;
    };
    require(drawn(110, 80, "Gabor 12 L3") == "Gabor 12 L3", "text that fits was changed");
    // 26 px left at x = 274: three bytes do not fit with the ellipsis, "..." alone does.
    require(drawn(274, 80, "Slug M7/8") == "...", "long label at the right edge was not elided to fit");
    require(drawn(250, 80, "Slug M7/8") == "Slug...", "long label was not elided at the right edge");
    require(drawn(290, 80, "Slug M7/8") == "<dropped>", "label with no room for the ellipsis was drawn");
    require(drawn(110, 50 + kEditorTextAscent, "Top") == "Top", "text touching the top from inside was dropped");
    require(drawn(110, 50 + kEditorTextAscent - 1, "Top") == "<dropped>", "text whose glyphs cross the top was drawn");
    require(drawn(110, 150 - kEditorTextDescent, "Bottom") == "Bottom", "text touching the bottom from inside was dropped");
    require(drawn(110, 150 - kEditorTextDescent + 1, "Bottom") == "<dropped>", "text whose descenders cross the bottom was drawn");
    require(drawn(90, 80, "Left") == "<dropped>", "text anchored left of the clip was drawn");
    require(drawn(300, 80, "Right") == "<dropped>", "text anchored right of the clip was drawn");
    std::printf("text clipping: OK\n");
}

} // namespace

int main() {
    try {
        test_text_stays_in_clip();
        test_large_orbit_stays_in_viewport();
        test_orbit_sweep_stays_in_viewport();
        std::printf("editor viewport clipping tests passed\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
