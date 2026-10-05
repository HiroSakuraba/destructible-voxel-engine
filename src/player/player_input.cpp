#include "dve/player/player_input.hpp"

#include "dve/game_world.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <utility>

namespace dve::player {
namespace {

using platform::GamepadAxis;
using platform::GamepadButton;
using platform::PointerButton;

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::optional<GamepadButton> gamepad_button(std::string_view name) {
    static const std::pair<std::string_view, GamepadButton> table[] = {
        {"south", GamepadButton::South}, {"a", GamepadButton::South},
        {"east", GamepadButton::East}, {"b", GamepadButton::East},
        {"west", GamepadButton::West}, {"x", GamepadButton::West},
        {"north", GamepadButton::North}, {"y", GamepadButton::North},
        {"back", GamepadButton::Back}, {"guide", GamepadButton::Guide}, {"start", GamepadButton::Start},
        {"leftstick", GamepadButton::LeftStick}, {"rightstick", GamepadButton::RightStick},
        {"leftshoulder", GamepadButton::LeftShoulder}, {"rightshoulder", GamepadButton::RightShoulder},
        {"dpup", GamepadButton::DpadUp}, {"dpdown", GamepadButton::DpadDown},
        {"dpleft", GamepadButton::DpadLeft}, {"dpright", GamepadButton::DpadRight},
    };
    for (const auto& [key, value] : table) if (key == name) return value;
    return std::nullopt;
}

std::optional<GamepadAxis> gamepad_axis(std::string_view name) {
    static const std::pair<std::string_view, GamepadAxis> table[] = {
        {"leftx", GamepadAxis::LeftX}, {"lefty", GamepadAxis::LeftY},
        {"rightx", GamepadAxis::RightX}, {"righty", GamepadAxis::RightY},
        {"lefttrigger", GamepadAxis::LeftTrigger}, {"righttrigger", GamepadAxis::RightTrigger},
    };
    for (const auto& [key, value] : table) if (key == name) return value;
    return std::nullopt;
}

std::optional<PointerButton> mouse_button(std::string_view name) {
    if (name == "left") return PointerButton::Primary;
    if (name == "middle") return PointerButton::Auxiliary;
    if (name == "right") return PointerButton::Secondary;
    if (name == "x1") return PointerButton::Extra1;
    if (name == "x2") return PointerButton::Extra2;
    return std::nullopt;
}

bool parse_float(std::string_view text, float& value) {
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && std::isfinite(value);
}

std::optional<InputSource> parse_source(std::string_view text, std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<InputSource> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    text = trim(text);
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos) return fail("source '" + std::string(text) + "' needs <device>:<control>");
    const std::string device = lower(trim(text.substr(0, colon)));
    std::string_view control = trim(text.substr(colon + 1U));
    InputSource source;
    if (const std::size_t slash = control.rfind('/'); slash != std::string_view::npos && slash > 0U) {
        if (!parse_float(trim(control.substr(slash + 1U)), source.value) || std::abs(source.value) > 1000.0F)
            return fail("source '" + std::string(text) + "' has an invalid /value");
        source.explicitValue = true;
        control = trim(control.substr(0, slash));
    }
    const std::string name = lower(control);
    if (name.empty() || name.find('/') != std::string::npos)
        return fail("source '" + std::string(text) + "' has an empty or invalid control");
    if (device == "key") {
        source.kind = InputSourceKind::Key;
        source.key = name;
    } else if (device == "gamepad") {
        if (const auto axis = gamepad_axis(name)) {
            source.kind = InputSourceKind::GamepadAxis;
            source.gamepadAxis = *axis;
        } else if (const auto button = gamepad_button(name)) {
            source.kind = InputSourceKind::GamepadButton;
            source.gamepadButton = *button;
        } else {
            return fail("unknown gamepad control '" + name + "'");
        }
    } else if (device == "mouse") {
        const auto button = mouse_button(name);
        if (!button) return fail("unknown mouse button '" + name + "'");
        source.kind = InputSourceKind::MouseButton;
        source.mouseButton = *button;
    } else {
        return fail("unknown input device '" + device + "' (expected key, gamepad or mouse)");
    }
    return source;
}

} // namespace

std::optional<InputBindingTable> InputBindingTable::parse(
    const std::map<std::string, std::string>& bindings, std::string* error) {
    InputBindingTable table;
    for (const auto& [name, spec] : bindings) {
        InputBinding binding;
        binding.name = name;
        std::string_view rest = spec;
        while (true) {
            const std::size_t comma = rest.find(',');
            const std::string_view item = rest.substr(0, comma);
            std::string sourceError;
            auto source = parse_source(item, &sourceError);
            if (!source) {
                if (error) *error = "bind." + name + ": " + sourceError;
                return std::nullopt;
            }
            binding.axis = binding.axis || source->explicitValue || source->kind == InputSourceKind::GamepadAxis;
            binding.sources.push_back(std::move(*source));
            if (comma == std::string_view::npos) break;
            rest.remove_prefix(comma + 1U);
        }
        table.bindings_.push_back(std::move(binding));
    }
    return table;
}

const InputBinding* InputBindingTable::find(std::string_view name) const noexcept {
    for (const InputBinding& binding : bindings_) if (binding.name == name) return &binding;
    return nullptr;
}

void PlayerInput::handle_event(const platform::PlatformEvent& event) {
    using platform::EventType;
    switch (event.type) {
        case EventType::KeyDown:
            if (!event.repeat) keys_.insert(lower(event.key));
            break;
        case EventType::KeyUp: keys_.erase(lower(event.key)); break;
        case EventType::GamepadButtonDown: gamepadButtons_.insert(event.gamepadButton); break;
        case EventType::GamepadButtonUp: gamepadButtons_.erase(event.gamepadButton); break;
        case EventType::GamepadAxisMotion: gamepadAxes_[event.gamepadAxis] = event.gamepadValue; break;
        case EventType::GamepadRemoved:
            gamepadButtons_.clear();
            gamepadAxes_.clear();
            break;
        case EventType::PointerButtonDown: mouseButtons_.insert(event.button); break;
        case EventType::PointerButtonUp: mouseButtons_.erase(event.button); break;
        case EventType::WindowFocusLost: release_all(); break;
        default: break;
    }
}

void PlayerInput::release_all() {
    keys_.clear();
    gamepadButtons_.clear();
    gamepadAxes_.clear();
    mouseButtons_.clear();
}

float PlayerInput::source_value(const InputSource& source, bool* held) const {
    *held = false;
    switch (source.kind) {
        case InputSourceKind::Key: *held = keys_.contains(source.key); break;
        case InputSourceKind::GamepadButton: *held = gamepadButtons_.contains(source.gamepadButton); break;
        case InputSourceKind::MouseButton: *held = mouseButtons_.contains(source.mouseButton); break;
        case InputSourceKind::GamepadAxis: {
            const auto it = gamepadAxes_.find(source.gamepadAxis);
            if (it == gamepadAxes_.end() || std::abs(it->second) < kGamepadAxisDeadZone) return 0.0F;
            const float value = it->second * source.value;
            *held = std::abs(value) >= 0.5F;
            return value;
        }
    }
    return *held ? source.value : 0.0F;
}

float PlayerInput::axis(std::string_view name) const {
    const InputBinding* binding = table_.find(name);
    if (binding == nullptr) return 0.0F;
    float sum = 0.0F;
    for (const InputSource& source : binding->sources) {
        bool held = false;
        sum += source_value(source, &held);
    }
    return std::clamp(sum, -1.0F, 1.0F);
}

bool PlayerInput::action(std::string_view name) const {
    const InputBinding* binding = table_.find(name);
    if (binding == nullptr) return false;
    for (const InputSource& source : binding->sources) {
        bool held = false;
        (void)source_value(source, &held);
        if (held) return true;
    }
    return false;
}

void PlayerInput::apply(GameWorld& world) const {
    for (const InputBinding& binding : table_.bindings()) {
        if (binding.axis) {
            world.set_axis(binding.name, axis(binding.name));
            world.set_action_pressed(binding.name, std::abs(axis(binding.name)) >= 0.5F);
        } else {
            world.set_action_pressed(binding.name, action(binding.name));
        }
    }
}

} // namespace dve::player
