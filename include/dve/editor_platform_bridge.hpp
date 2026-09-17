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

private:
    NativeEditorController& controller_;
};

} // namespace dve::editor
