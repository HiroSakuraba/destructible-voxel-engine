// Main editor chrome legibility at every supported window size and UI zoom:
//   * every toolbar label stays inside its button and is never empty,
//   * hovering a toolbar button shows its name, real shortcut and description,
//   * the viewport hint shows only the active tool's gestures and never runs under the
//     camera preview; the "? Shortcuts" control is inside the viewport and opens the guide,
//   * the camera preview title stays inside the preview, eliding a long rig name,
//   * menu rows keep the label and the shortcut column apart, and a hovered row that is cut
//     or unavailable explains itself in a tooltip,
//   * inspector details that do not fit above the flag toggles scroll instead of being cut.
#include "dve/editor_native.hpp"
#include "dve/editor_native_renderer.hpp"
#include "dve/editor_ui_zoom.hpp"

#include <algorithm>
#include <memory>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace dve;
using namespace dve::editor;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}

int code_points(std::string_view value) {
    int count = 0;
    for (const char c : value) if ((static_cast<unsigned char>(c) & 0xC0U) != 0x80U) ++count;
    return count;
}

struct DrawnText { int x; int y; std::string value; int width; };
// 7 px per code point: a little wider than the 11 px DejaVu Sans Mono UI font, so a pass here
// leaves margin on screen.
class MeasuringCanvas final : public IEditorCanvas {
public:
    void fill(UiRect rect, EditorColor) const override { fills.push_back({rect, texts.size()}); }
    void outline(UiRect, EditorColor) const override {}
    void line(int, int, int, int, EditorColor, int) const override {}
    void text(int x, int y, std::string_view value, EditorColor) const override {
        texts.push_back({x, y, std::string(value), text_width(value)});
    }
    [[nodiscard]] int text_width(std::string_view value) const override { return code_points(value) * 7; }
    [[nodiscard]] const DrawnText* find(std::string_view needle) const {
        for (const DrawnText& t : texts) if (t.value.find(needle) != std::string::npos) return &t;
        return nullptr;
    }
    [[nodiscard]] const DrawnText* first_at(int y, const UiRect& within) const {
        for (const DrawnText& t : texts) if (t.y == y && within.contains(t.x, t.y - 4)) return &t;
        return nullptr;
    }
    // Index of the first text drawn after the last fill of exactly `rect` (what is on top of it).
    [[nodiscard]] std::size_t texts_after_fill(const UiRect& rect) const {
        std::size_t index = 0;
        for (const auto& [filled, at] : fills)
            if (filled.x == rect.x && filled.y == rect.y && filled.width == rect.width && filled.height == rect.height)
                index = at;
        return index;
    }
    mutable std::vector<DrawnText> texts;
    mutable std::vector<std::pair<UiRect, std::size_t>> fills;
};

bool inside_x(const DrawnText& t, const UiRect& r) { return t.x >= r.x && t.x + t.width <= r.x + r.width; }
bool contains_origin(const UiRect& r, const DrawnText& t) { return r.contains(t.x, t.y - 4); }

struct Case { int w; int h; std::string tag; };
std::vector<Case> cases() {
    std::vector<Case> result;
    for (auto [w, h] : std::vector<std::pair<int, int>>{{1280, 720}, {1536, 960}, {1366, 768}, {1920, 1080},
                                                         {2560, 1440}, {800, 600}, {640, 480}}) {
        for (float zoom : {1.0F, 1.25F, 1.5F, 1.75F, 2.0F}) {
            const float effective = effective_ui_zoom(zoom, w, h);
            const int lw = ui_zoom_logical_extent(w, effective);
            const int lh = ui_zoom_logical_extent(h, effective);
            result.push_back({lw, lh, std::to_string(w) + "x" + std::to_string(h) + "@" +
                                          std::to_string(static_cast<int>(zoom * 100)) + "% (" +
                                          std::to_string(lw) + "x" + std::to_string(lh) + ") "});
        }
    }
    return result;
}

std::unique_ptr<NativeEditorController> make_controller_ptr(const Case& c) {
    auto controller = std::make_unique<NativeEditorController>(EditorWorkspace(make_native_editor_demo_document()));
    controller->resize(c.w, c.h);
    return controller;
}
#define MAKE_CONTROLLER(name, ...) auto name##Owner = make_controller_ptr(__VA_ARGS__); NativeEditorController& name = *name##Owner

// Opens a top-level menu the way a user does: a click on its title in the menu bar.
void open_menu(NativeEditorController& controller, std::size_t index, int width) {
    const int menuWidth = std::max(1, std::min(78, width / static_cast<int>(kMenuBarNames.size())));
    const int x = static_cast<int>(index) * menuWidth + menuWidth / 2;
    if (controller.open_menu() && *controller.open_menu() == kMenuBarNames[index]) return;
    controller.pointer_down(PointerButton::Primary, x, 10);
    controller.pointer_up(PointerButton::Primary, x, 10);
}

void test_toolbar(const Case& c) {
    MAKE_CONTROLLER(controller, c);
    MeasuringCanvas canvas;
    render_native_editor(canvas, controller, c.w, c.h);
    const auto& buttons = controller.layout().toolbarButtons;
    check(buttons.size() == kEditorToolCount, c.tag + "nine toolbar buttons");
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        const UiRect& b = buttons[i];
        check(b.x + b.width <= c.w, c.tag + "toolbar button " + std::to_string(i) + " leaves the window");
        if (i + 1 < buttons.size()) check(b.x + b.width <= buttons[i + 1].x, c.tag + "toolbar buttons overlap");
        int labels = 0;
        for (const DrawnText& t : canvas.texts) {
            if (!contains_origin(b, t)) continue;
            ++labels;
            check(!t.value.empty(), c.tag + "empty toolbar label");
            check(inside_x(t, b), c.tag + "toolbar label '" + t.value + "' leaves its button " + std::to_string(i));
        }
        check(labels == 1, c.tag + "toolbar button " + std::to_string(i) + " has " + std::to_string(labels) + " labels");
    }
    // At the default 1280x720 every tool shows its full name.
    if (c.tag.starts_with("1280x720@100%")) {
        for (std::size_t i = 0; i < kEditorToolCount; ++i) {
            const auto name = editor_tool_info(static_cast<EditorToolId>(i)).name;
            const DrawnText* t = canvas.find(name);
            check(t && t->value == name, c.tag + "full toolbar name '" + std::string(name) + "' is shown");
        }
    }
}

void test_toolbar_tooltips() {
    const Case c{1280, 720, "1280x720 "};
    MAKE_CONTROLLER(controller, c);
    for (std::size_t i = 0; i < kEditorToolCount; ++i) {
        const auto tool = static_cast<EditorToolId>(i);
        const UiRect b = controller.layout().toolbarButtons[i];
        controller.pointer_move(b.x + b.width / 2, b.y + b.height / 2);
        MeasuringCanvas canvas;
        render_native_editor(canvas, controller, c.w, c.h);
        const EditorToolInfo& info = editor_tool_info(tool);
        const std::string key = controller.tool_shortcut_text(tool);
        const std::string heading = std::string(info.name) + (key.empty() ? "  (no shortcut)" : "  (" + key + ")");
        check(canvas.find(heading) != nullptr, "tooltip heading '" + heading + "'");
        check(canvas.find(info.description) != nullptr, "tooltip description for " + std::string(info.name));
        check(info.actionId.empty() == key.empty(), std::string(info.name) + " shortcut presence matches its action");
    }
    // The shortcuts in the tooltips are the real bindings, not the old "1".."9" prefixes.
    check(controller.tool_shortcut_text(EditorToolId::Select) == "Q", "Select is Q");
    check(controller.tool_shortcut_text(EditorToolId::Translate) == "W", "Move is W");
    check(controller.tool_shortcut_text(EditorToolId::Rotate) == "E", "Rotate is E");
    check(controller.tool_shortcut_text(EditorToolId::Scale) == "R", "Scale is R");
}

void test_viewport_hint(const Case& c) {
    MAKE_CONTROLLER(controller, c);
    const UiRect viewport = controller.layout().viewport;
    for (std::size_t i = 0; i < kEditorToolCount; ++i) {
        controller.set_active_tool(static_cast<EditorToolId>(i));
        const auto gestures = controller.viewport_tool_gestures();
        check(!gestures.empty() && gestures.size() <= 3, c.tag + "two or three gestures per tool");
        MeasuringCanvas canvas;
        render_native_editor(canvas, controller, c.w, c.h);
        const DrawnText* hint = canvas.first_at(viewport.y + 20, viewport);
        const DrawnText* title = canvas.find("[Preview]");
        if (!hint) {
            // Allowed only when not even the main gesture fits left of the preview.
            const int room = (title ? title->x - 7 - 10 : viewport.x + viewport.width - 12) - (viewport.x + 12);
            check(canvas.text_width(gestures.front()) > room,
                  c.tag + "hint missing although '" + gestures.front() + "' fits");
            continue;
        }
        check(hint->value.starts_with(gestures.front()), c.tag + "hint starts with the main gesture, got '" + hint->value + "'");
        check(hint->value.find("T move") == std::string::npos, c.tag + "stale 'T move' hint");
        check(hint->x + hint->width <= viewport.x + viewport.width, c.tag + "hint leaves the viewport");
        if (title && title->y < viewport.y + 40)
            check(hint->x + hint->width <= title->x - 7, c.tag + "hint '" + hint->value + "' runs under the camera preview");
    }
    const UiRect help = controller.layout().viewportHelpButton;
    check(help.width > 0, c.tag + "shortcut help control present");
    check(help.x >= viewport.x && help.x + help.width <= viewport.x + viewport.width &&
              help.y >= viewport.y && help.y + help.height <= viewport.y + viewport.height,
          c.tag + "help control inside the viewport");
}

void test_help_control_opens_guide() {
    MAKE_CONTROLLER(controller, {1280, 720, ""});
    const UiRect help = controller.layout().viewportHelpButton;
    check(!controller.shortcut_panel().open, "shortcut guide starts closed");
    controller.pointer_down(PointerButton::Primary, help.x + help.width / 2, help.y + help.height / 2);
    controller.pointer_up(PointerButton::Primary, help.x + help.width / 2, help.y + help.height / 2);
    check(controller.shortcut_panel().open, "'? Shortcuts' opens the shortcut guide");
}

void test_camera_preview_title() {
    const Case c{640, 480, "640x480 "};
    MAKE_CONTROLLER(controller, c);
    auto rig = controller.selected_camera_rig();
    check(rig.has_value(), "demo document selects a camera rig");
    if (!rig) return;
    camera::CameraRig* rigData = controller.camera_director().find_rig(*rig);
    check(rigData != nullptr, "selected rig exists");
    if (!rigData) return;
    rigData->name = "Cinematic establishing shot over the harbour at dawn";
    MeasuringCanvas canvas;
    render_native_editor(canvas, controller, c.w, c.h);
    const DrawnText* title = canvas.find("[Preview]");
    check(title != nullptr, "preview title keeps its tag");
    if (!title) return;
    check(title->value.find("…") != std::string::npos, "long rig name is elided, got '" + title->value + "'");
    const UiRect viewport = controller.layout().viewport;
    check(title->x + title->width <= viewport.x + viewport.width, "preview title stays inside the viewport");
    controller.pointer_move(title->x + 4, title->y - 4);
    MeasuringCanvas hovered;
    render_native_editor(hovered, controller, c.w, c.h);
    check(hovered.find("Cinematic establishing shot over the harbour at dawn") != nullptr,
          "hovering the elided title shows the full rig name");
}

void test_menus(const Case& c) {
    MAKE_CONTROLLER(controller, c);
    for (std::size_t m = 0; m < kMenuBarNames.size(); ++m) {
        const std::string_view menu = kMenuBarNames[m];
        open_menu(controller, m, c.w);
        check(controller.open_menu() && *controller.open_menu() == menu, c.tag + "opened menu " + std::string(menu));
        const NativeMenuPopupLayout popup = controller.menu_popup_layout();
        MeasuringCanvas canvas;
        render_native_editor(canvas, controller, c.w, c.h);
        for (const UiRect& row : popup.rows) {
            std::vector<const DrawnText*> rowTexts;
            for (std::size_t k = canvas.texts_after_fill(popup.popup); k < canvas.texts.size(); ++k)
                if (const DrawnText& t = canvas.texts[k]; t.y == row.y + row.height - 7 && t.x >= popup.popup.x && t.x < popup.popup.x + popup.popup.width)
                    rowTexts.push_back(&t);
            std::sort(rowTexts.begin(), rowTexts.end(), [](auto* a, auto* b) { return a->x < b->x; });
            for (std::size_t i = 0; i < rowTexts.size(); ++i) {
                check(rowTexts[i]->x + rowTexts[i]->width <= popup.popup.x + popup.popup.width,
                      c.tag + std::string(menu) + " row text '" + rowTexts[i]->value + "' leaves the popup");
                if (i + 1 < rowTexts.size())
                    check(rowTexts[i]->x + rowTexts[i]->width <= rowTexts[i + 1]->x,
                          c.tag + std::string(menu) + " label '" + rowTexts[i]->value + "' runs into '" +
                              rowTexts[i + 1]->value + "'");
            }
        }
    }
}

void test_disabled_menu_reason() {
    MAKE_CONTROLLER(controller, {1280, 720, ""});
    bool explained = false;
    for (std::size_t m = 0; m < kMenuBarNames.size(); ++m) {
        const std::string_view menu = kMenuBarNames[m];
        open_menu(controller, m, 1280);
        const auto actions = controller.menu_actions(menu);
        const NativeMenuPopupLayout popup = controller.menu_popup_layout();
        for (std::size_t v = 0; v < popup.rows.size() && !explained; ++v) {
            const std::size_t i = popup.firstVisibleAction + v;
            if (i >= actions.size() || actions[i].enabled || actions[i].disabledReason.empty()) continue;
            const UiRect row = popup.rows[v];
            controller.pointer_move(row.x + row.width / 2, row.y + row.height / 2);
            MeasuringCanvas canvas;
            render_native_editor(canvas, controller, 1280, 720);
            explained = canvas.find("Unavailable: " + actions[i].disabledReason) != nullptr;
            check(explained, "hovering unavailable '" + actions[i].label + "' shows why");
        }
        if (explained) break;
    }
    check(explained, "found an unavailable menu command to explain");
}

void test_inspector_scroll() {
    // 800x600: the voxel inspector's details do not fit above the flag toggles.
    MAKE_CONTROLLER(controller, {800, 600, ""});
    const NativeEditorLayout& layout = controller.layout();
    check(layout.inspectorScrollMax > 0, "800x600 inspector needs scrolling");
    const UiRect inspector = layout.inspector;
    const auto render_texts = [&] {
        MeasuringCanvas canvas;
        render_native_editor(canvas, controller, 800, 600);
        return canvas;
    };
    MeasuringCanvas before = render_texts();
    check(before.find("Layer ") == nullptr, "last detail line starts below the fold");
    for (int i = 0; i < 20; ++i)
        controller.pointer_wheel(-1.0F, inspector.x + inspector.width / 2, inspector.y + inspector.height / 2);
    check(controller.layout().inspectorScroll == controller.layout().inspectorScrollMax, "wheel scrolls to the end");
    MeasuringCanvas after = render_texts();
    const DrawnText* layer = after.find("Layer ");
    check(layer != nullptr, "scrolling reveals the last detail line");
    for (const DrawnText& t : after.texts) {
        if (!inspector.contains(t.x, t.y - 4)) continue;
        if (t.value == "INSPECTOR") continue;
        const bool toggle = std::any_of(controller.layout().inspectorToggles.begin(),
                                        controller.layout().inspectorToggles.end(),
                                        [&](const UiRect& r) { return r.contains(t.x, t.y - 4); });
        if (toggle) continue;
        check(t.y - kEditorTextAscent >= inspector.y + kInspectorHeaderHeight,
              "scrolled detail '" + t.value + "' draws over the inspector header");
        check(t.y + kEditorTextDescent <= controller.layout().inspectorContentClipY,
              "scrolled detail '" + t.value + "' draws over the toggles");
    }
    // Editable fields move with the text, and disappear when scrolled away.
    const auto& fields = controller.layout().inspectorFields;
    check(!fields.empty() && (fields[0].width == 0 || fields[0].y >= inspector.y + kInspectorHeaderHeight),
          "position field follows the scroll");
    for (int i = 0; i < 20; ++i)
        controller.pointer_wheel(1.0F, inspector.x + inspector.width / 2, inspector.y + inspector.height / 2);
    check(controller.layout().inspectorScroll == 0, "wheel scrolls back to the top");
    // A tall window needs no scrolling.
    MAKE_CONTROLLER(tall, {1280, 1024, ""});
    check(tall.layout().inspectorScrollMax == 0, "1280x1024 inspector fits without scrolling");
}

} // namespace

int main() {
    for (const Case& c : cases()) {
        test_toolbar(c);
        test_viewport_hint(c);
        test_menus(c);
    }
    test_toolbar_tooltips();
    test_help_control_opens_guide();
    test_camera_preview_title();
    test_disabled_menu_reason();
    test_inspector_scroll();
    if (g_failures != 0) {
        std::printf("dve_editor_chrome_fit_tests: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("dve_editor_chrome_fit_tests: PASS\n");
    return 0;
}
