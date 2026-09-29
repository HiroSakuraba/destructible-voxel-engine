#include "dve/editor_ui_zoom.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace dve::editor {

float snap_ui_zoom(float requested) noexcept {
    if (!std::isfinite(requested)) return kUiZoomDefault;
    const float clamped = std::clamp(requested, kUiZoomMin, kUiZoomMax);
    return std::clamp(std::round(clamped / kUiZoomStep) * kUiZoomStep, kUiZoomMin, kUiZoomMax);
}

float max_ui_zoom_for_window(int physicalWidth, int physicalHeight) noexcept {
    if (physicalWidth <= 0 || physicalHeight <= 0) return kUiZoomMin;
    const float fit = std::min(static_cast<float>(physicalWidth) / static_cast<float>(kUiZoomMinLogicalWidth),
                               static_cast<float>(physicalHeight) / static_cast<float>(kUiZoomMinLogicalHeight));
    // Small epsilon so an exact fit (e.g. 1280x960 at 2.0) is not lost to rounding.
    const float stepped = std::floor(fit / kUiZoomStep + 1.0e-4F) * kUiZoomStep;
    return std::clamp(stepped, kUiZoomMin, kUiZoomMax);
}

float effective_ui_zoom(float requested, int physicalWidth, int physicalHeight) noexcept {
    return std::min(snap_ui_zoom(requested), max_ui_zoom_for_window(physicalWidth, physicalHeight));
}

float step_ui_zoom(float current, int direction) noexcept {
    if (direction == 0) return kUiZoomDefault;
    return snap_ui_zoom(snap_ui_zoom(current) + (direction > 0 ? kUiZoomStep : -kUiZoomStep));
}

int ui_zoom_to_physical(int logical, float zoom) noexcept {
    // Edges use ceil so they are the exact inverse of the flooring pointer mapping below:
    // physical pixel p lies inside logical [a, b) iff ceil(a*z) <= p < ceil(b*z).
    return static_cast<int>(std::ceil(static_cast<double>(logical) * static_cast<double>(zoom) - 1.0e-9));
}

int ui_zoom_to_logical(int physical, float zoom) noexcept {
    if (!(zoom > 0.0F)) return physical;
    return static_cast<int>(std::floor(static_cast<double>(physical) / static_cast<double>(zoom)));
}

int ui_zoom_logical_extent(int physicalExtent, float zoom) noexcept {
    return std::max(1, ui_zoom_to_logical(physicalExtent, zoom));
}

UiRect ui_zoom_to_physical(const UiRect& logical, float zoom) noexcept {
    const int left = ui_zoom_to_physical(logical.x, zoom);
    const int top = ui_zoom_to_physical(logical.y, zoom);
    const int right = ui_zoom_to_physical(logical.x + logical.width, zoom);
    const int bottom = ui_zoom_to_physical(logical.y + logical.height, zoom);
    return {left, top, right - left, bottom - top};
}

int ui_zoom_stroke(float logicalWidth, float zoom) noexcept {
    const float width = std::isfinite(logicalWidth) ? logicalWidth : 1.0F;
    return std::max(1, static_cast<int>(std::lround(width * zoom)));
}

int ui_zoom_text_pixel_size(float zoom) noexcept {
    return std::max(kUiBaseTextPixels, static_cast<int>(std::lround(static_cast<float>(kUiBaseTextPixels) * snap_ui_zoom(zoom))));
}

std::string format_ui_zoom_percent(float zoom) {
    return std::to_string(static_cast<int>(std::lround(zoom * 100.0F))) + "%";
}

std::optional<int> ui_zoom_hotkey_direction(std::string_view key, bool control, bool alt) noexcept {
    if (!control || alt) return std::nullopt;
    std::string name;
    name.reserve(key.size());
    for (char c : key) name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (name == "=" || name == "equal" || name == "+" || name == "plus" || name == "kp_add" || name == "keypad +")
        return 1;
    if (name == "-" || name == "minus" || name == "kp_subtract" || name == "keypad -") return -1;
    if (name == "0" || name == "kp_0" || name == "keypad 0" || name == "numpad0") return 0;
    return std::nullopt;
}

} // namespace dve::editor
