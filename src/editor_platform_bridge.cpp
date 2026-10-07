#include "dve/editor_platform_bridge.hpp"
#include "dve/editor_ui_zoom.hpp"
#include "dve/editor_runtime_settings.hpp"

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

EditorPlatformBridge::~EditorPlatformBridge() {
    if (host_) (void)host_->set_relative_mouse_mode(false);
}

void EditorPlatformBridge::sync_pointer_capture() {
    const bool navigation = controller_.navigation_pointer_active();
    if (!navigation && navigationWasActive_) controller_.clear_navigation_input();
    navigationWasActive_ = navigation;
    if (!host_) return;
    const bool wanted = controller_.raw_mouse_requested() && navigation;
    if (!wanted) {
        if (host_->relative_mouse_mode()) {
            (void)host_->set_relative_mouse_mode(false);
            controller_.clear_navigation_input();
        }
        captureAttempted_ = false;
    } else if (!captureAttempted_) {
        captureAttempted_ = true;
        std::string error;
        if (!host_->set_relative_mouse_mode(true, &error))
            controller_.workspace().log().add(EditorLogLevel::Warning,
                "Raw mouse unavailable; using ordinary pointer motion: " + error);
    }
}

void EditorPlatformBridge::handle_event(const platform::PlatformEvent& event) {
    const bool control = platform::has_modifier(event.modifiers, platform::Modifier::Control);
    const bool shift = platform::has_modifier(event.modifiers, platform::Modifier::Shift);
    const bool alt = platform::has_modifier(event.modifiers, platform::Modifier::Alt);
    const int x = ui_zoom_to_logical(event.x, zoom_);
    const int y = ui_zoom_to_logical(event.y, zoom_);

    switch (event.type) {
        case platform::EventType::QuitRequested:
            controller_.request_quit();
            break;
        case platform::EventType::WindowResized:
            controller_.set_ui_zoom_window_limit(max_ui_zoom_for_window(event.width, event.height));
            controller_.resize(ui_zoom_logical_extent(event.width, zoom_), ui_zoom_logical_extent(event.height, zoom_));
            break;
        case platform::EventType::KeyDown:
            if (event.key == "Up" || event.key == "Down" || event.key == "Left" || event.key == "Right") {
                const bool ui = controller_.settings_panel().open ||
                    controller_.focus_region() != EditorFocusRegion::Viewport;
                if (ui) {
                    if (event.repeat || heldNavigation_.contains(event.key)) break;
                    const RuntimeSettingsReader settings(controller_.workspace().settings());
                    heldNavigation_.emplace(event.key, HeldNavigationKey{event, 0.0,
                        settings.number("input.ui_repeat_delay")});
                }
            }
            controller_.key_down(event.key, control, shift, alt);
            break;
        case platform::EventType::KeyUp:
            heldNavigation_.erase(event.key);
            controller_.key_up(event.key, control, shift, alt);
            break;
        case platform::EventType::TextInput:
            controller_.text_input(event.text);
            break;
        case platform::EventType::PointerMove:
            if (event.relativeMotion) controller_.pointer_relative(event.deltaX, event.deltaY);
            else controller_.pointer_move(x, y, pointer_modifiers(event.modifiers));
            break;
        case platform::EventType::PointerButtonDown:
            if (!host_ || !host_->relative_mouse_mode()) { capturePointerX_ = x; capturePointerY_ = y; }
            controller_.pointer_down(to_editor_button(event.button), x, y,
                                     pointer_modifiers(event.modifiers));
            break;
        case platform::EventType::PointerButtonUp:
            controller_.pointer_up(to_editor_button(event.button),
                                   host_ && host_->relative_mouse_mode() ? capturePointerX_ : x,
                                   host_ && host_->relative_mouse_mode() ? capturePointerY_ : y,
                                   pointer_modifiers(event.modifiers));
            break;
        case platform::EventType::PointerWheel:
            controller_.pointer_wheel(event.wheelY, x, y, pointer_modifiers(event.modifiers));
            break;
        case platform::EventType::WindowFocusLost:
            for (const auto& [key, held] : heldNavigation_) {
                const auto mods = held.event.modifiers;
                controller_.key_up(key, platform::has_modifier(mods, platform::Modifier::Control),
                    platform::has_modifier(mods, platform::Modifier::Shift), platform::has_modifier(mods, platform::Modifier::Alt));
            }
            heldNavigation_.clear();
            controller_.clear_navigation_input();
            rawSticks_ = {};
            sticksDirty_ = true;
            break;
        case platform::EventType::GamepadRemoved:
            if (activeGamepad_ == event.gamepadId) {
                activeGamepad_.reset();
                rawSticks_ = {};
                sticksDirty_ = true;
                controller_.set_gamepad_axes({}, {});
            }
            break;
        case platform::EventType::NoEvent:
        case platform::EventType::WindowFocusGained:
        case platform::EventType::TextEditing:
        case platform::EventType::FileDropped:
        case platform::EventType::GamepadAdded:
        case platform::EventType::GamepadButtonDown:
        case platform::EventType::GamepadButtonUp:
            break;
        case platform::EventType::GamepadAxisMotion: {
            const auto axis = event.gamepadAxis;
            const int index = axis == platform::GamepadAxis::LeftX ? 0
                : axis == platform::GamepadAxis::LeftY ? 1
                : axis == platform::GamepadAxis::RightX ? 2
                : axis == platform::GamepadAxis::RightY ? 3 : -1;
            if (index < 0) break; // triggers have a separate, unipolar policy
            if (!activeGamepad_) activeGamepad_ = event.gamepadId;
            if (*activeGamepad_ != event.gamepadId) break;
            rawSticks_[static_cast<std::size_t>(index)] = std::isfinite(event.gamepadValue)
                ? std::clamp(event.gamepadValue, -1.0F, 1.0F) : 0.0F;
            sticksDirty_ = true;
            publish_gamepad_state();
            break;
        }
    }
    sync_pointer_capture();
}

void EditorPlatformBridge::publish_gamepad_state() {
    if (!controller_.play_session().active()) return;
    const RuntimeSettingsReader settings(controller_.workspace().settings());
    const float zone = settings.number("input.controller_dead_zone");
    const auto move = controller_stick_with_dead_zone(rawSticks_[0], rawSticks_[1], zone);
    const auto look = controller_stick_with_dead_zone(rawSticks_[2], rawSticks_[3], zone);
    // SDL's down-positive stick Y maps to the keyboard's forward-positive move_y.
    controller_.set_gamepad_axes({move[0], -move[1]}, look);
    stickSettingsRevision_ = controller_.workspace().settings().revision();
    sticksDirty_ = false;
}

void EditorPlatformBridge::update(float elapsedSeconds) {
    sync_pointer_capture();
    const bool playing = controller_.play_session().active();
    if (playing && (sticksDirty_ || !playWasActive_ ||
        stickSettingsRevision_ != controller_.workspace().settings().revision())) publish_gamepad_state();
    playWasActive_ = playing;
    if (heldNavigation_.empty()) return;
    if (!std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0F) return;
    if (!controller_.settings_panel().open && controller_.focus_region() == EditorFocusRegion::Viewport) {
        heldNavigation_.clear();
        return;
    }
    const RuntimeSettingsReader settings(controller_.workspace().settings());
    const double interval = settings.number("input.ui_repeat_rate");
    for (auto& [key, held] : heldNavigation_) {
        held.elapsed += elapsedSeconds;
        if (held.elapsed < held.nextRepeat) continue;
        const auto mods = held.event.modifiers;
        controller_.key_down(key, platform::has_modifier(mods, platform::Modifier::Control),
            platform::has_modifier(mods, platform::Modifier::Shift), platform::has_modifier(mods, platform::Modifier::Alt));
        // A stalled frame produces one navigation event, never a catch-up burst.
        held.nextRepeat = held.elapsed + interval;
    }
}

} // namespace dve::editor
