#pragma once

#include "dve/editor_native.hpp"
#include "dve/platform/application_host.hpp"

namespace dve::editor {

// Routes the shared platform event vocabulary into the existing toolkit-neutral editor
// controller. Platform backends must normalize key names to the same strings used by the menu
// registry (for example "Escape", "Enter", "C", "F5").
class EditorPlatformBridge {
public:
    explicit EditorPlatformBridge(NativeEditorController& controller) : controller_(controller) {}

    void handle_event(const platform::PlatformEvent& event);
    // UI zoom currently applied by the host (see dve/editor_ui_zoom.hpp). Pointer positions and
    // resize extents arrive in window coordinates and are mapped to logical editor pixels.
    void set_ui_zoom(float zoom) noexcept { zoom_ = zoom > 0.0F ? zoom : 1.0F; }
    [[nodiscard]] float ui_zoom() const noexcept { return zoom_; }

private:
    NativeEditorController& controller_;
    float zoom_{1.0F};
};

} // namespace dve::editor
