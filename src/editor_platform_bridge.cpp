#include "dve/editor_platform_bridge.hpp"

namespace dve::editor {
namespace {

PointerButton to_editor_button(platform::PointerButton button) noexcept {
    switch (button) {
        case platform::PointerButton::Primary: return PointerButton::Primary;
        case platform::PointerButton::Auxiliary: return PointerButton::Auxiliary;
        case platform::PointerButton::Secondary: return PointerButton::Secondary;
        case platform::PointerButton::Extra1: return PointerButton::Extra1;
        default: return PointerButton::NoButton;
    }
}

std::uint32_t pointer_modifiers(platform::Modifier modifiers) noexcept {
    // Preserve the controller's established bit contract: Shift=1, Control=2, Alt=4.
    std::uint32_t result = 0U;
    if (platform::has_modifier(modifiers, platform::Modifier::Shift)) result |= 1U;
    if (platform::has_modifier(modifiers, platform::Modifier::Control)) result |= 2U;
    if (platform::has_modifier(modifiers, platform::Modifier::Alt)) result |= 4U;
    return result;
}

} // namespace

void EditorPlatformBridge::handle_event(const platform::PlatformEvent& event) {
    const bool control = platform::has_modifier(event.modifiers, platform::Modifier::Control);
    const bool shift = platform::has_modifier(event.modifiers, platform::Modifier::Shift);
    const bool alt = platform::has_modifier(event.modifiers, platform::Modifier::Alt);

    switch (event.type) {
        case platform::EventType::QuitRequested:
            controller_.request_quit();
            break;
        case platform::EventType::WindowResized:
            controller_.resize(event.width, event.height);
            break;
        case platform::EventType::KeyDown:
            controller_.key_down(event.key, control, shift, alt);
            break;
        case platform::EventType::KeyUp:
            controller_.key_up(event.key, control, shift, alt);
            break;
        case platform::EventType::TextInput:
            controller_.text_input(event.text);
            break;
        case platform::EventType::PointerMove:
            controller_.pointer_move(event.x, event.y, pointer_modifiers(event.modifiers));
            break;
        case platform::EventType::PointerButtonDown:
            controller_.pointer_down(to_editor_button(event.button), event.x, event.y,
                                     pointer_modifiers(event.modifiers));
            break;
        case platform::EventType::PointerButtonUp:
            controller_.pointer_up(to_editor_button(event.button), event.x, event.y,
                                   pointer_modifiers(event.modifiers));
            break;
        case platform::EventType::PointerWheel:
            controller_.pointer_wheel(event.wheelY, event.x, event.y, pointer_modifiers(event.modifiers));
            break;
        case platform::EventType::NoEvent:
        case platform::EventType::WindowFocusGained:
        case platform::EventType::WindowFocusLost:
        case platform::EventType::TextEditing:
        case platform::EventType::FileDropped:
        case platform::EventType::GamepadAdded:
        case platform::EventType::GamepadRemoved:
        case platform::EventType::GamepadButtonDown:
        case platform::EventType::GamepadButtonUp:
        case platform::EventType::GamepadAxisMotion:
            break;
    }
}

} // namespace dve::editor
