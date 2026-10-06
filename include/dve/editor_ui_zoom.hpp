#pragma once

// UI zoom (accessibility scale) shared by every native editor host (X11, SDL).
//
// The editor controller lays out and hit-tests in *logical* pixels. A host with a
// physical window of W x H pixels runs the controller at floor(W / zoom) x floor(H / zoom)
// logical pixels, maps every draw call logical -> physical, maps pointer input
// physical -> logical, and rasterizes text at text_pixel_size(zoom) so it stays crisp.
//
// There is one persisted zoom value: the User-scope setting `editor.ui_scale`
// (labelled "UI Zoom" in Settings, 100-200 % in 25 % steps). The hotkeys
// (Ctrl+= / Ctrl+- / Ctrl+0), the settings row and both hosts all go through it.

#include <optional>
#include <string>
#include <string_view>

#include "dve/editor_viewport.hpp"

namespace dve::editor {

inline constexpr float kUiZoomMin = 1.0F;
inline constexpr float kUiZoomMax = 2.0F;
inline constexpr float kUiZoomStep = 0.25F;
inline constexpr float kUiZoomDefault = 1.0F;
// Smallest logical canvas the controller supports; zoom is capped so the
// logical size never drops below this (NativeEditorController::resize floor).
inline constexpr int kUiZoomMinLogicalWidth = 640;
inline constexpr int kUiZoomMinLogicalHeight = 480;
// Base editor text size in logical pixels (the old fixed 6x13 bitmap font was ~11 px tall).
inline constexpr int kUiBaseTextPixels = 11;
inline constexpr std::string_view kUiZoomSettingId = "editor.ui_scale";

// Clamp to [kUiZoomMin, kUiZoomMax] and snap to the nearest 25 % step.
// Non-finite input yields the default. Legacy values (e.g. 0.75, 1.1, 3.0) are snapped, not rejected.
[[nodiscard]] float snap_ui_zoom(float requested) noexcept;
// Largest step-aligned zoom that keeps the logical canvas >= 640x480 for this window (>= kUiZoomMin).
[[nodiscard]] float max_ui_zoom_for_window(int physicalWidth, int physicalHeight) noexcept;
// The zoom a host should actually apply: snap_ui_zoom(requested) capped by the window size.
[[nodiscard]] float effective_ui_zoom(float requested, int physicalWidth, int physicalHeight) noexcept;
// Next zoom for a hotkey: direction > 0 zooms in one step, < 0 out one step, 0 resets to 100 %.
[[nodiscard]] float step_ui_zoom(float current, int direction) noexcept;

// Logical <-> physical mapping. Rect edges are mapped independently (ceil) so adjacent
// rectangles stay seamless at fractional zoom; pointer mapping floors, which makes it the
// exact inverse: every physical pixel drawn for a logical rect hit-tests inside that rect.
[[nodiscard]] int ui_zoom_to_physical(int logical, float zoom) noexcept;
[[nodiscard]] int ui_zoom_to_logical(int physical, float zoom) noexcept;
[[nodiscard]] int ui_zoom_logical_extent(int physicalExtent, float zoom) noexcept;
[[nodiscard]] UiRect ui_zoom_to_physical(const UiRect& logical, float zoom) noexcept;
// Line / stroke thickness in physical pixels for a logical width (>= 1).
[[nodiscard]] int ui_zoom_stroke(float logicalWidth, float zoom) noexcept;
// Text raster size in physical pixels for the given zoom (base 11 px, never smaller).
[[nodiscard]] int ui_zoom_text_pixel_size(float zoom) noexcept;
// "150%"
[[nodiscard]] std::string format_ui_zoom_percent(float zoom);

// Recognizes the fixed UI-zoom hotkeys independent of keyboard layout naming:
// Ctrl+= / Ctrl++ / Ctrl+KP_Add -> +1, Ctrl+- / Ctrl+KP_Subtract -> -1, Ctrl+0 / Ctrl+KP_0 -> 0.
// Key names are X11 keysym names or SDL key names (case-insensitive). Alt must not be held.
[[nodiscard]] std::optional<int> ui_zoom_hotkey_direction(std::string_view key, bool control, bool alt) noexcept;

} // namespace dve::editor
