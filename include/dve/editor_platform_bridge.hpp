#pragma once

#include "dve/editor_native.hpp"
#include "dve/platform/application_host.hpp"

namespace dve::editor {

// Routes the shared platform event vocabulary into the existing toolkit-neutral editor
// controller. Platform backends must normalize key names to the same strings used by the menu
// registry (for example "Escape", "Enter", "C", "F5").
class EditorPlatformBridge {
public:
    explicit EditorPlatformBridge(NativeEditorController& controller, platform::IApplicationHost* host = nullptr)
        : controller_(controller), host_(host) {}
    ~EditorPlatformBridge();

    void handle_event(const platform::PlatformEvent& event);
    void update(float elapsedSeconds);
    void sync_pointer_capture();
    // UI zoom currently applied by the host (see dve/editor_ui_zoom.hpp). Pointer positions and
    // resize extents arrive in window coordinates and are mapped to logical editor pixels.
    void set_ui_zoom(float zoom) noexcept { zoom_ = zoom > 0.0F ? zoom : 1.0F; }
    [[nodiscard]] float ui_zoom() const noexcept { return zoom_; }

private:
    NativeEditorController& controller_;
    platform::IApplicationHost* host_{};
    bool captureAttempted_{};
    bool navigationWasActive_{};
    int capturePointerX_{}, capturePointerY_{};
    float zoom_{1.0F};
    struct HeldNavigationKey {
        platform::PlatformEvent event;
        double elapsed{};
        double nextRepeat{};
    };
    std::map<std::string, HeldNavigationKey, std::less<>> heldNavigation_;
    void publish_gamepad_state();
    std::array<float, 4> rawSticks_{};
    std::optional<std::int32_t> activeGamepad_;
    std::uint64_t stickSettingsRevision_{};
    bool sticksDirty_{};
    bool playWasActive_{};
};

} // namespace dve::editor
