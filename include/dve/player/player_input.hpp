#pragma once
// Input bindings for dve_player, interpreted from game.dvegame `bind.<name>=<spec>` entries
// (decision D6; the v2.35 InputActionSystem migration is a follow-up).
//
//   bind.move_x=key:a/-1,key:d/+1,gamepad:leftx     -> axis "move_x"
//   bind.jump=key:space,gamepad:south,mouse:left     -> action "jump"
//
// A spec is a comma-separated list of sources:
//   key:<name>[/<value>]        SDL key name, lower case ("a", "space", "left", "left shift")
//   gamepad:<button>[/<value>]  south east west north back guide start leftstick rightstick
//                               leftshoulder rightshoulder dpup dpdown dpleft dpright
//   gamepad:<axis>[/<scale>]    leftx lefty rightx righty lefttrigger righttrigger
//   mouse:<button>[/<value>]    left middle right x1 x2
// A binding is an *axis* if any source carries an explicit /value or is a gamepad axis:
// its value is the clamped [-1, 1] sum of held keys/buttons' values and axis*scale (with a
// 0.15 dead zone). Otherwise it is an *action*, pressed while any source is held, and for
// the tick after a press even if the source was released before that tick ran.
// Every frame the player pushes the state into GameWorld::set_axis / set_action_pressed,
// which scripts read with world.get_axis / world.is_action_pressed.
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "dve/platform/application_host.hpp"

namespace dve {
class GameWorld;
}

namespace dve::player {

enum class InputSourceKind : std::uint8_t { Key, GamepadButton, GamepadAxis, MouseButton };

struct InputSource {
    InputSourceKind kind{InputSourceKind::Key};
    std::string key;                                    // Key
    platform::GamepadButton gamepadButton{platform::GamepadButton::Unknown};
    platform::GamepadAxis gamepadAxis{platform::GamepadAxis::Unknown};
    platform::PointerButton mouseButton{platform::PointerButton::NoButton};
    float value{1.0F};
    bool explicitValue{};
};

struct InputBinding {
    std::string name;
    bool axis{};
    std::vector<InputSource> sources;
};

inline constexpr float kGamepadAxisDeadZone = 0.15F;

class InputBindingTable {
public:
    // Parses GameManifest::inputBindings. Errors name the binding and the bad source.
    [[nodiscard]] static std::optional<InputBindingTable> parse(
        const std::map<std::string, std::string>& bindings, std::string* error = nullptr);
    [[nodiscard]] const std::vector<InputBinding>& bindings() const noexcept { return bindings_; }
    [[nodiscard]] const InputBinding* find(std::string_view name) const noexcept;

private:
    std::vector<InputBinding> bindings_;
};

class PlayerInput {
public:
    PlayerInput() = default;
    explicit PlayerInput(InputBindingTable table) : table_(std::move(table)) {}

    // Tracks key / gamepad / mouse state. Key repeats are ignored; focus loss releases all.
    void handle_event(const platform::PlatformEvent& event);
    void release_all();
    // Ends the input window for one simulation tick. Actions report a press that happened
    // since the previous end_tick() even if it was already released, so a tap shorter than a
    // tick (common at high refresh rates, where most frames run no tick) is not lost. Call it
    // after the tick has read the input (PlayerApp::tick does). Axes report held state only.
    void end_tick();

    [[nodiscard]] float axis(std::string_view name) const;
    [[nodiscard]] bool action(std::string_view name) const;
    // Pushes every binding into the world (set_axis for axes, set_action_pressed for actions).
    void apply(GameWorld& world) const;

    [[nodiscard]] const InputBindingTable& table() const noexcept { return table_; }

private:
    [[nodiscard]] float source_value(const InputSource& source, bool* held) const;

    InputBindingTable table_;
    std::set<std::string> keys_;
    std::set<platform::GamepadButton> gamepadButtons_;
    std::map<platform::GamepadAxis, float> gamepadAxes_;
    std::set<platform::PointerButton> mouseButtons_;
    // Pressed since the last end_tick(), whether or not still held.
    std::set<std::string> pressedKeys_;
    std::set<platform::GamepadButton> pressedGamepadButtons_;
    std::set<platform::PointerButton> pressedMouseButtons_;
};

} // namespace dve::player
