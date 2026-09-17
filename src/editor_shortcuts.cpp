#include "dve/editor_shortcuts.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <set>
#include <sstream>
#include <utility>

namespace dve::editor {
namespace {

std::string lowercase(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return result;
}

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1U]))) --end;
    return std::string(text.substr(begin, end - begin));
}

std::string canonical_input(std::string_view input) {
    std::string value = lowercase(trim(input));
    if (value == "return") value = "enter";
    else if (value == "kp_0") value = "numpad0";
    else if (value == "kp_1") value = "numpad1";
    else if (value == "kp_3") value = "numpad3";
    else if (value == "kp_7") value = "numpad7";
    else if (value == "braceleft") value = "[";
    else if (value == "braceright") value = "]";
    else if (value == "equal") value = "=";
    else if (value == "minus") value = "-";
    else if (value == "button1") value = "mouse1";
    else if (value == "button2") value = "mouse3";
    else if (value == "button3") value = "mouse2";
    else if (value == "button8" || value == "x1") value = "mouse4";
    return value;
}

std::string title_input(std::string_view input) {
    const std::string value = canonical_input(input);
    if (value == "mouse1") return "LMB";
    if (value == "mouse2") return "RMB";
    if (value == "mouse3") return "MMB";
    if (value == "mouse4") return "Mouse4";
    if (value == "wheelup") return "Wheel Up";
    if (value == "wheeldown") return "Wheel Down";
    if (value == "enter") return "Enter";
    if (value == "escape") return "Esc";
    if (value == "delete") return "Delete";
    if (value == "backspace") return "Backspace";
    if (value == "space") return "Space";
    if (value == "left") return "Left";
    if (value == "right") return "Right";
    if (value == "up") return "Up";
    if (value == "down") return "Down";
    if (value == "pageup") return "Page Up";
    if (value == "pagedown") return "Page Down";
    if (value == "home") return "Home";
    if (value == "end") return "End";
    if (value.starts_with("numpad")) {
        std::string result = "Numpad";
        result += value.substr(6U);
        return result;
    }
    if (value.size() == 1U && std::isalpha(static_cast<unsigned char>(value[0])))
        return std::string(1U, static_cast<char>(std::toupper(static_cast<unsigned char>(value[0]))));
    if (value.size() >= 2U && value[0] == 'f' &&
        std::all_of(value.begin() + 1, value.end(), [](unsigned char c) { return std::isdigit(c); })) {
        std::string result = value;
        result[0] = 'F';
        return result;
    }
    return value;
}

bool command_supports_context(const ShortcutCommandDefinition& command, ShortcutContext context) {
    return std::find(command.contexts.begin(), command.contexts.end(), context) != command.contexts.end();
}

int context_rank(ShortcutContext context) noexcept {
    switch (context) {
        case ShortcutContext::ModalDialog: return 130;
        case ShortcutContext::TextField: return 120;
        case ShortcutContext::PlayMode: return 110;
        case ShortcutContext::FlyNavigation: return 105;
        case ShortcutContext::Sequencer: return 100;
        case ShortcutContext::AudioEditor: return 95;
        case ShortcutContext::VoxelEditor: return 90;
        case ShortcutContext::PolygonEditor: return 90;
        case ShortcutContext::MaterialEditor: return 90;
        case ShortcutContext::Camera: return 85;
        case ShortcutContext::AssetBrowser: return 80;
        case ShortcutContext::Viewport: return 70;
        case ShortcutContext::Global: return 0;
    }
    return 0;
}

void add_keywords(ShortcutCommandDefinition& command, std::initializer_list<std::string_view> words) {
    for (std::string_view word : words) command.keywords.emplace_back(word);
}

} // namespace

std::string shortcut_context_name(ShortcutContext context) {
    switch (context) {
        case ShortcutContext::Global: return "Global";
        case ShortcutContext::Viewport: return "Viewport";
        case ShortcutContext::FlyNavigation: return "Fly Navigation";
        case ShortcutContext::PlayMode: return "Play Mode";
        case ShortcutContext::Camera: return "Camera";
        case ShortcutContext::Sequencer: return "Sequencer";
        case ShortcutContext::AudioEditor: return "Audio Editor";
        case ShortcutContext::VoxelEditor: return "Voxel Editor";
        case ShortcutContext::PolygonEditor: return "Polygon Editor";
        case ShortcutContext::MaterialEditor: return "Material Editor";
        case ShortcutContext::AssetBrowser: return "Asset Browser";
        case ShortcutContext::TextField: return "Text Field";
        case ShortcutContext::ModalDialog: return "Modal Dialog";
    }
    return "Global";
}

std::optional<ShortcutContext> parse_shortcut_context(std::string_view text) {
    const std::string value = lowercase(trim(text));
    for (int raw = static_cast<int>(ShortcutContext::Global);
         raw <= static_cast<int>(ShortcutContext::ModalDialog); ++raw) {
        const auto context = static_cast<ShortcutContext>(raw);
        if (lowercase(shortcut_context_name(context)) == value) return context;
    }
    if (value == "fly") return ShortcutContext::FlyNavigation;
    if (value == "audio") return ShortcutContext::AudioEditor;
    if (value == "voxel") return ShortcutContext::VoxelEditor;
    if (value == "polygon") return ShortcutContext::PolygonEditor;
    if (value == "material") return ShortcutContext::MaterialEditor;
    return std::nullopt;
}

ShortcutGesture keyboard_shortcut(std::string key, bool control, bool shift, bool alt,
                                  ShortcutActivation activation) {
    return {ShortcutDevice::Keyboard, activation, canonical_input(key), control, shift, alt};
}

ShortcutGesture mouse_shortcut(std::string button, bool control, bool shift, bool alt,
                               ShortcutActivation activation) {
    return {ShortcutDevice::MouseButton, activation, canonical_input(button), control, shift, alt};
}

ShortcutGesture wheel_shortcut(bool up, bool control, bool shift, bool alt) {
    return {ShortcutDevice::Wheel, ShortcutActivation::Press, up ? "wheelup" : "wheeldown",
            control, shift, alt};
}

std::string format_shortcut_gesture(const ShortcutGesture& gesture) {
    if (!gesture.valid()) return {};
    std::string result;
    if (gesture.activation == ShortcutActivation::Hold) result += "Hold ";
    else if (gesture.activation == ShortcutActivation::DoubleClick) result += "Double ";
    if (gesture.control) result += "Ctrl+";
    if (gesture.shift) result += "Shift+";
    if (gesture.alt) result += "Alt+";
    result += title_input(gesture.input);
    return result;
}

std::optional<ShortcutGesture> parse_shortcut_gesture(std::string_view text, std::string* error) {
    auto fail = [&](std::string message) -> std::optional<ShortcutGesture> {
        if (error != nullptr) *error = std::move(message);
        return std::nullopt;
    };
    std::string value = trim(text);
    if (value.empty() || value == "-") return std::nullopt;
    ShortcutGesture result;
    const std::string lower = lowercase(value);
    if (lower.starts_with("hold ")) {
        result.activation = ShortcutActivation::Hold;
        value.erase(0, 5U);
    } else if (lower.starts_with("double ")) {
        result.activation = ShortcutActivation::DoubleClick;
        value.erase(0, 7U);
    }
    std::stringstream stream(value);
    std::string token;
    std::vector<std::string> tokens;
    while (std::getline(stream, token, '+')) tokens.push_back(trim(token));
    if (tokens.empty()) return fail("shortcut gesture has no input");
    for (std::size_t index = 0; index + 1U < tokens.size(); ++index) {
        const std::string modifier = lowercase(tokens[index]);
        if (modifier == "ctrl" || modifier == "control") result.control = true;
        else if (modifier == "shift") result.shift = true;
        else if (modifier == "alt") result.alt = true;
        else return fail("unknown shortcut modifier: " + tokens[index]);
    }
    result.input = canonical_input(tokens.back());
    if (result.input == "lmb") result.input = "mouse1";
    else if (result.input == "rmb") result.input = "mouse2";
    else if (result.input == "mmb") result.input = "mouse3";
    const std::string inputLower = lowercase(tokens.back());
    if (inputLower == "lmb" || inputLower == "rmb" || inputLower == "mmb" ||
        inputLower == "mouse4" || inputLower == "mouse1" || inputLower == "mouse2" ||
        inputLower == "mouse3") result.device = ShortcutDevice::MouseButton;
    else if (result.input == "wheelup" || result.input == "wheeldown" ||
             inputLower == "wheel up" || inputLower == "wheel down") {
        result.device = ShortcutDevice::Wheel;
        result.input = inputLower.find("up") != std::string::npos ? "wheelup" : "wheeldown";
    } else result.device = ShortcutDevice::Keyboard;
    if (!result.valid()) return fail("shortcut gesture has no input");
    return result;
}

bool EditorShortcutRegistry::add_command(ShortcutCommandDefinition command, std::string* error) {
    auto fail = [&](std::string message) { if (error != nullptr) *error = std::move(message); return false; };
    if (command.actionId.empty()) return fail("shortcut command id cannot be empty");
    if (command.label.empty()) return fail("shortcut command label cannot be empty");
    if (find_command(command.actionId) != nullptr) return fail("duplicate shortcut command: " + command.actionId);
    if (command.contexts.empty()) command.contexts.push_back(ShortcutContext::Global);
    std::sort(command.contexts.begin(), command.contexts.end());
    command.contexts.erase(std::unique(command.contexts.begin(), command.contexts.end()), command.contexts.end());
    commands_.push_back(std::move(command));
    return true;
}

const ShortcutCommandDefinition* EditorShortcutRegistry::find_command(std::string_view actionId) const noexcept {
    const auto found = std::find_if(commands_.begin(), commands_.end(), [&](const ShortcutCommandDefinition& item) {
        return item.actionId == actionId;
    });
    return found == commands_.end() ? nullptr : &*found;
}

bool EditorShortcutRegistry::has_profile(std::string_view name) const noexcept {
    return profiles_.contains(std::string(name));
}

std::vector<std::string> EditorShortcutRegistry::profile_names() const {
    std::vector<std::string> result;
    result.reserve(profiles_.size());
    for (const auto& [name, bindingsValue] : profiles_) {
        (void)bindingsValue;
        result.push_back(name);
    }
    return result;
}

bool EditorShortcutRegistry::set_active_profile(std::string_view name, std::string* error) {
    if (!has_profile(name)) {
        if (error != nullptr) *error = "unknown shortcut profile: " + std::string(name);
        return false;
    }
    activeProfile_ = std::string(name);
    return true;
}

bool EditorShortcutRegistry::duplicate_profile(std::string_view source, std::string_view destination,
                                                std::string* error) {
    if (destination.empty()) {
        if (error != nullptr) *error = "shortcut profile name cannot be empty";
        return false;
    }
    const auto found = profiles_.find(std::string(source));
    if (found == profiles_.end()) {
        if (error != nullptr) *error = "source shortcut profile does not exist";
        return false;
    }
    if (profiles_.contains(std::string(destination))) {
        if (error != nullptr) *error = "destination shortcut profile already exists";
        return false;
    }
    profiles_.emplace(std::string(destination), found->second);
    return true;
}

bool EditorShortcutRegistry::remove_profile(std::string_view name, std::string* error) {
    if (profiles_.size() <= 1U) {
        if (error != nullptr) *error = "cannot remove the last shortcut profile";
        return false;
    }
    if (builtinDefaults_.contains(std::string(name))) {
        if (error != nullptr) *error = "built-in shortcut profiles cannot be removed";
        return false;
    }
    if (profiles_.erase(std::string(name)) == 0U) {
        if (error != nullptr) *error = "shortcut profile does not exist";
        return false;
    }
    if (activeProfile_ == name) activeProfile_ = profiles_.begin()->first;
    return true;
}

bool EditorShortcutRegistry::reset_profile(std::string_view name, std::string* error) {
    const auto defaults = builtinDefaults_.find(std::string(name));
    if (defaults == builtinDefaults_.end()) {
        if (error != nullptr) *error = "profile has no built-in defaults";
        return false;
    }
    profiles_[std::string(name)] = defaults->second;
    return true;
}

bool EditorShortcutRegistry::contexts_overlap(ShortcutContext a, ShortcutContext b) const noexcept {
    if (a == b) return true;
    // Context-specific commands intentionally override global fallbacks while that context
    // owns input; this is the core mechanism that lets Space play a timeline but toggle
    // transform space in the viewport without reporting a false conflict.
    if (a == ShortcutContext::Global || b == ShortcutContext::Global) return false;
    if ((a == ShortcutContext::FlyNavigation && b == ShortcutContext::Viewport) ||
        (b == ShortcutContext::FlyNavigation && a == ShortcutContext::Viewport)) return true;
    if ((a == ShortcutContext::Camera && b == ShortcutContext::Viewport) ||
        (b == ShortcutContext::Camera && a == ShortcutContext::Viewport)) return true;
    return false;
}

bool EditorShortcutRegistry::gesture_conflicts(std::string_view profile, std::string_view actionId,
                                                ShortcutContext context, const ShortcutGesture& gesture,
                                                std::vector<ShortcutConflict>* conflictsOut) const {
    const auto foundProfile = profiles_.find(std::string(profile));
    if (foundProfile == profiles_.end()) return false;
    bool conflict = false;
    for (const auto& [key, pair] : foundProfile->second) {
        if (key.actionId == actionId && key.context == context) continue;
        if (!contexts_overlap(context, key.context)) continue;
        const auto check = [&](const std::optional<ShortcutGesture>& candidate) {
            if (!candidate || *candidate != gesture) return;
            conflict = true;
            if (conflictsOut != nullptr) {
                conflictsOut->push_back({std::string(actionId), key.actionId, context, gesture,
                    "The contexts '" + shortcut_context_name(context) + "' and '" +
                    shortcut_context_name(key.context) + "' can be active together."});
            }
        };
        check(pair.primary);
        check(pair.secondary);
    }
    return conflict;
}

bool EditorShortcutRegistry::set_binding(std::string_view profile, std::string_view actionId,
                                          ShortcutContext context, ShortcutSlot slot,
                                          std::optional<ShortcutGesture> gesture,
                                          bool overrideConflicts, std::string* error) {
    auto fail = [&](std::string message) { if (error != nullptr) *error = std::move(message); return false; };
    auto foundProfile = profiles_.find(std::string(profile));
    if (foundProfile == profiles_.end()) return fail("shortcut profile does not exist");
    const ShortcutCommandDefinition* command = find_command(actionId);
    if (command == nullptr) return fail("shortcut command does not exist: " + std::string(actionId));
    if (!command_supports_context(*command, context)) return fail("command is not valid in the requested context");
    if (gesture) {
        gesture->input = canonical_input(gesture->input);
        if (!gesture->valid()) return fail("shortcut gesture is empty");
        std::vector<ShortcutConflict> foundConflicts;
        if (gesture_conflicts(profile, actionId, context, *gesture, &foundConflicts)) {
            if (!overrideConflicts) {
                return fail("shortcut conflicts with " + foundConflicts.front().otherActionId + " in " +
                            shortcut_context_name(foundConflicts.front().context));
            }
            for (auto& [otherKey, otherPair] : foundProfile->second) {
                if (!contexts_overlap(context, otherKey.context)) continue;
                if (otherPair.primary && *otherPair.primary == *gesture) otherPair.primary.reset();
                if (otherPair.secondary && *otherPair.secondary == *gesture) otherPair.secondary.reset();
            }
        }
    }
    ShortcutBindingPair& pair = foundProfile->second[{std::string(actionId), context}];
    if (slot == ShortcutSlot::Primary) pair.primary = std::move(gesture);
    else pair.secondary = std::move(gesture);
    return true;
}

ShortcutBindingPair EditorShortcutRegistry::bindings(std::string_view profile, std::string_view actionId,
                                                       ShortcutContext context) const {
    const auto foundProfile = profiles_.find(std::string(profile));
    if (foundProfile == profiles_.end()) return {};
    const auto found = foundProfile->second.find({std::string(actionId), context});
    return found == foundProfile->second.end() ? ShortcutBindingPair{} : found->second;
}

std::optional<ShortcutResolution> EditorShortcutRegistry::resolve(
    const ShortcutGesture& gesture, std::span<const ShortcutContext> activeContexts,
    std::uint32_t capabilities) const {
    const auto foundProfile = profiles_.find(activeProfile_);
    if (foundProfile == profiles_.end()) return std::nullopt;
    std::vector<ShortcutContext> ordered(activeContexts.begin(), activeContexts.end());
    if (std::find(ordered.begin(), ordered.end(), ShortcutContext::Global) == ordered.end())
        ordered.push_back(ShortcutContext::Global);
    std::stable_sort(ordered.begin(), ordered.end(), [](ShortcutContext a, ShortcutContext b) {
        return context_rank(a) > context_rank(b);
    });
    ordered.erase(std::unique(ordered.begin(), ordered.end()), ordered.end());
    for (ShortcutContext context : ordered) {
        for (const ShortcutCommandDefinition& command : commands_) {
            if ((command.requiredCapabilities & capabilities) != command.requiredCapabilities) continue;
            if (!command_supports_context(command, context)) continue;
            const ShortcutBindingPair pair = bindings(activeProfile_, command.actionId, context);
            if (pair.primary && *pair.primary == gesture)
                return ShortcutResolution{command.actionId, context, ShortcutSlot::Primary};
            if (pair.secondary && *pair.secondary == gesture)
                return ShortcutResolution{command.actionId, context, ShortcutSlot::Secondary};
        }
    }
    return std::nullopt;
}

std::vector<ShortcutConflict> EditorShortcutRegistry::conflicts(std::string_view profile) const {
    std::vector<ShortcutConflict> result;
    const auto foundProfile = profiles_.find(std::string(profile));
    if (foundProfile == profiles_.end()) return result;
    std::set<std::string> seen;
    for (const auto& [key, pair] : foundProfile->second) {
        for (const auto& gesture : {pair.primary, pair.secondary}) {
            if (!gesture) continue;
            std::vector<ShortcutConflict> local;
            (void)gesture_conflicts(profile, key.actionId, key.context, *gesture, &local);
            for (ShortcutConflict& conflict : local) {
                const std::string first = std::min(conflict.actionId, conflict.otherActionId);
                const std::string second = std::max(conflict.actionId, conflict.otherActionId);
                const std::string signature = first + "\n" + second + "\n" +
                    shortcut_context_name(conflict.context) + "\n" + format_shortcut_gesture(conflict.gesture);
                if (seen.insert(signature).second) result.push_back(std::move(conflict));
            }
        }
    }
    return result;
}

std::vector<ShortcutSearchResult> EditorShortcutRegistry::search(
    std::string_view query, std::optional<ShortcutContext> context,
    std::uint32_t capabilities, std::size_t limit) const {
    const std::string needle = lowercase(trim(query));
    std::vector<ShortcutSearchResult> result;
    for (const ShortcutCommandDefinition& command : commands_) {
        if ((command.requiredCapabilities & capabilities) != command.requiredCapabilities) continue;
        std::vector<ShortcutContext> contexts = command.contexts;
        if (context) {
            if (!command_supports_context(command, *context)) continue;
            contexts = {*context};
        }
        int score = 0;
        const std::string id = lowercase(command.actionId);
        const std::string label = lowercase(command.label);
        const std::string category = lowercase(command.category);
        if (needle.empty()) score = 1;
        else {
            if (label == needle || id == needle) score += 100;
            if (label.starts_with(needle) || id.starts_with(needle)) score += 50;
            if (label.find(needle) != std::string::npos || id.find(needle) != std::string::npos) score += 25;
            if (category.find(needle) != std::string::npos) score += 12;
            for (const std::string& keyword : command.keywords)
                if (lowercase(keyword).find(needle) != std::string::npos) score += 8;
            for (ShortcutContext itemContext : contexts) {
                const ShortcutBindingPair pair = bindings(command.actionId, itemContext);
                if ((pair.primary && lowercase(format_shortcut_gesture(*pair.primary)).find(needle) != std::string::npos) ||
                    (pair.secondary && lowercase(format_shortcut_gesture(*pair.secondary)).find(needle) != std::string::npos))
                    score += 30;
            }
        }
        if (score <= 0) continue;
        for (ShortcutContext itemContext : contexts)
            result.push_back({&command, bindings(command.actionId, itemContext), itemContext, score});
    }
    std::stable_sort(result.begin(), result.end(), [](const ShortcutSearchResult& a, const ShortcutSearchResult& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.command->category != b.command->category) return a.command->category < b.command->category;
        if (a.command->label != b.command->label) return a.command->label < b.command->label;
        return a.context < b.context;
    });
    if (result.size() > limit) result.resize(limit);
    return result;
}

std::vector<ShortcutKeyboardKeyVisual> EditorShortcutRegistry::keyboard_map(
    std::optional<ShortcutContext> context) const {
    std::map<std::string, ShortcutKeyboardKeyVisual, std::less<>> map;
    const auto conflictList = conflicts(activeProfile_);
    const auto isConflicted = [&](std::string_view action, const ShortcutGesture& gesture) {
        return std::any_of(conflictList.begin(), conflictList.end(), [&](const ShortcutConflict& conflict) {
            return (conflict.actionId == action || conflict.otherActionId == action) && conflict.gesture == gesture;
        });
    };
    const auto foundProfile = profiles_.find(activeProfile_);
    if (foundProfile == profiles_.end()) return {};
    for (const auto& [key, pair] : foundProfile->second) {
        if (context && key.context != *context) continue;
        for (const auto& gesture : {pair.primary, pair.secondary}) {
            if (!gesture || gesture->device != ShortcutDevice::Keyboard) continue;
            ShortcutKeyboardKeyVisual& visual = map[gesture->input];
            visual.key = title_input(gesture->input);
            visual.actions.push_back(key.actionId + " [" + shortcut_context_name(key.context) + "]");
            visual.conflicted = visual.conflicted || isConflicted(key.actionId, *gesture);
        }
    }
    std::vector<ShortcutKeyboardKeyVisual> result;
    result.reserve(map.size());
    for (auto& [key, visual] : map) {
        (void)key;
        result.push_back(std::move(visual));
    }
    return result;
}

std::string EditorShortcutRegistry::display_binding(
    std::string_view actionId, std::span<const ShortcutContext> preferredContexts) const {
    for (ShortcutContext context : preferredContexts) {
        const ShortcutBindingPair pair = bindings(actionId, context);
        if (pair.primary) return format_shortcut_gesture(*pair.primary);
        if (pair.secondary) return format_shortcut_gesture(*pair.secondary);
    }
    const ShortcutCommandDefinition* command = find_command(actionId);
    if (command == nullptr) return {};
    for (ShortcutContext context : command->contexts) {
        const ShortcutBindingPair pair = bindings(actionId, context);
        if (pair.primary) return format_shortcut_gesture(*pair.primary);
        if (pair.secondary) return format_shortcut_gesture(*pair.secondary);
    }
    return {};
}

std::string EditorShortcutRegistry::serialize_profile(std::string_view profile) const {
    const auto found = profiles_.find(std::string(profile));
    if (found == profiles_.end()) return {};
    std::ostringstream output;
    output << "DVE_SHORTCUT_PROFILE=1\n";
    output << "name " << std::quoted(found->first) << "\n";
    for (const auto& [key, pair] : found->second) {
        output << "bind " << std::quoted(key.actionId) << ' '
               << std::quoted(shortcut_context_name(key.context)) << ' '
               << std::quoted(pair.primary ? format_shortcut_gesture(*pair.primary) : std::string{}) << ' '
               << std::quoted(pair.secondary ? format_shortcut_gesture(*pair.secondary) : std::string{}) << "\n";
    }
    return output.str();
}

bool EditorShortcutRegistry::parse_profile(std::string_view text, std::string* error) {
    auto fail = [&](std::string message) { if (error != nullptr) *error = std::move(message); return false; };
    std::istringstream input{std::string(text)};
    std::string line;
    if (!std::getline(input, line) || trim(line) != "DVE_SHORTCUT_PROFILE=1")
        return fail("shortcut profile header is missing or unsupported");
    std::string name;
    ProfileBindings parsed;
    bool sawName = false;
    while (std::getline(input, line)) {
        if (trim(line).empty()) continue;
        std::istringstream row(line);
        std::string kind;
        row >> kind;
        if (kind == "name") {
            if (sawName || !(row >> std::quoted(name)) || name.empty()) return fail("invalid shortcut profile name");
            sawName = true;
            std::string trailing;
            if (row >> trailing) return fail("trailing data after shortcut profile name");
            continue;
        }
        if (kind != "bind") return fail("unknown shortcut profile record: " + kind);
        std::string actionId;
        std::string contextText;
        std::string primaryText;
        std::string secondaryText;
        if (!(row >> std::quoted(actionId) >> std::quoted(contextText) >>
              std::quoted(primaryText) >> std::quoted(secondaryText)))
            return fail("malformed shortcut binding record");
        std::string trailing;
        if (row >> trailing) return fail("trailing data after shortcut binding record");
        const ShortcutCommandDefinition* command = find_command(actionId);
        if (command == nullptr) return fail("shortcut profile references unknown command: " + actionId);
        const auto context = parse_shortcut_context(contextText);
        if (!context || !command_supports_context(*command, *context))
            return fail("shortcut profile uses an invalid command context");
        ShortcutBindingPair pair;
        if (!primaryText.empty()) {
            std::string parseError;
            pair.primary = parse_shortcut_gesture(primaryText, &parseError);
            if (!pair.primary) return fail("invalid primary binding: " + parseError);
        }
        if (!secondaryText.empty()) {
            std::string parseError;
            pair.secondary = parse_shortcut_gesture(secondaryText, &parseError);
            if (!pair.secondary) return fail("invalid secondary binding: " + parseError);
        }
        if (!parsed.emplace(BindingKey{actionId, *context}, std::move(pair)).second)
            return fail("duplicate command/context binding in shortcut profile");
    }
    if (!sawName) return fail("shortcut profile has no name");
    const auto previous = profiles_.find(name);
    const std::optional<ProfileBindings> backup = previous == profiles_.end()
        ? std::nullopt : std::optional<ProfileBindings>(previous->second);
    profiles_[name] = std::move(parsed);
    const auto foundConflicts = conflicts(name);
    if (!foundConflicts.empty()) {
        if (backup) profiles_[name] = *backup;
        else profiles_.erase(name);
        return fail("shortcut profile contains conflicting bindings");
    }
    return true;
}

void EditorShortcutRegistry::install_builtin_commands() {
    const auto add = [&](std::string id, std::string label, std::string category,
                         std::initializer_list<ShortcutContext> contexts,
                         bool repeatable = false, bool destructive = false,
                         std::initializer_list<std::string_view> keywords = {}) {
        ShortcutCommandDefinition command;
        command.actionId = std::move(id);
        command.label = std::move(label);
        command.category = std::move(category);
        command.contexts.assign(contexts.begin(), contexts.end());
        command.repeatable = repeatable;
        command.destructive = destructive;
        add_keywords(command, keywords);
        std::string ignored;
        (void)add_command(std::move(command), &ignored);
    };

    add("window.toggle_ai_assistant", "Toggle AI Assistant", "Window", {ShortcutContext::Global});
    add("window.toggle_control_rig", "Toggle Control Rig Editor", "Window", {ShortcutContext::Global});
    add("window.toggle_chiptune", "Toggle Chiptune Tracker", "Window", {ShortcutContext::Global});
    add("render.diagnostics3d", "3D Rendering Diagnostics", "Rendering", {ShortcutContext::Global});
    add("asset.open", "Open Selected Asset", "Assets", {ShortcutContext::AssetBrowser});
    add("transform.select", "Select/View Tool", "Transform", {ShortcutContext::Viewport});
    add("transform.scale", "Scale Tool", "Transform", {ShortcutContext::Viewport});
    add("transform.universal", "Universal Transform Tool", "Transform", {ShortcutContext::Viewport});
    add("view.frame_all", "Frame Entire Scene", "Viewport", {ShortcutContext::Viewport});
    add("view.drop_to_surface", "Drop Selection to Surface", "Viewport", {ShortcutContext::Viewport});
    add("view.follow_selection", "Follow Selection", "Viewport", {ShortcutContext::Viewport, ShortcutContext::Camera});
    add("view.game_view", "Toggle Game View", "Viewport", {ShortcutContext::Viewport});
    add("view.realtime", "Toggle Realtime Viewport", "Viewport", {ShortcutContext::Viewport});
    add("view.immersive", "Toggle Immersive Viewport", "Viewport", {ShortcutContext::Viewport});
    add("view.wireframe", "Wireframe View", "Viewport", {ShortcutContext::Viewport});
    add("view.lit", "Lit View", "Viewport", {ShortcutContext::Viewport});
    add("view.unlit", "Unlit View", "Viewport", {ShortcutContext::Viewport});
    add("view.material_ids", "Material ID View", "Viewport", {ShortcutContext::Viewport});
    add("view.normals", "Normals and Tangents View", "Viewport", {ShortcutContext::Viewport});
    add("view.voxel_debug", "Voxel Debug View", "Viewport", {ShortcutContext::Viewport});
    add("view.lighting_only", "Lighting Only View", "Viewport", {ShortcutContext::Viewport});
    add("view.overdraw", "Overdraw Heatmap", "Viewport", {ShortcutContext::Viewport});
    add("view.previous_camera", "Previous Camera Position", "Camera", {ShortcutContext::Viewport, ShortcutContext::Camera});
    add("view.next_camera", "Next Camera Position", "Camera", {ShortcutContext::Viewport, ShortcutContext::Camera});
    add("view.zoom_in", "Zoom In", "Viewport", {ShortcutContext::Viewport}, true);
    add("view.zoom_out", "Zoom Out", "Viewport", {ShortcutContext::Viewport}, true);
    add("viewport.look", "Free Look", "Viewport Navigation", {ShortcutContext::Viewport}, false, false, {"RMB"});
    add("viewport.orbit", "Orbit Pivot", "Viewport Navigation", {ShortcutContext::Viewport}, false, false, {"Alt LMB"});
    add("viewport.pan", "Pan View", "Viewport Navigation", {ShortcutContext::Viewport}, false, false, {"MMB"});
    add("viewport.dolly", "Dolly View", "Viewport Navigation", {ShortcutContext::Viewport}, false, false, {"Alt RMB"});
    add("camera.fly_forward", "Fly Forward", "Viewport Navigation", {ShortcutContext::FlyNavigation}, true);
    add("camera.fly_backward", "Fly Backward", "Viewport Navigation", {ShortcutContext::FlyNavigation}, true);
    add("camera.fly_left", "Fly Left", "Viewport Navigation", {ShortcutContext::FlyNavigation}, true);
    add("camera.fly_right", "Fly Right", "Viewport Navigation", {ShortcutContext::FlyNavigation}, true);
    add("camera.fly_down", "Fly Down", "Viewport Navigation", {ShortcutContext::FlyNavigation}, true);
    add("camera.fly_up", "Fly Up", "Viewport Navigation", {ShortcutContext::FlyNavigation}, true);
    add("camera.create_from_view", "Create Camera from View", "Camera", {ShortcutContext::Camera, ShortcutContext::Viewport});
    add("camera.align_to_view", "Align Selected Camera to View", "Camera", {ShortcutContext::Camera, ShortcutContext::Viewport});
    add("camera.toggle_preview", "Toggle Selected Camera Preview", "Camera", {ShortcutContext::Camera, ShortcutContext::Viewport});
    add("camera.toggle_safe_frames", "Toggle Cinematic Safe Frames", "Camera", {ShortcutContext::Camera, ShortcutContext::Viewport});
    add("camera.lock", "Lock Selected Camera", "Camera", {ShortcutContext::Camera});
    add("camera.capture_screenshot", "Capture Viewport Screenshot", "Camera", {ShortcutContext::Camera, ShortcutContext::Viewport});
    add("camera.capture_selected", "Capture Selected Camera", "Camera", {ShortcutContext::Camera});
    for (int index = 1; index <= 9; ++index) {
        add("camera.save_bookmark_" + std::to_string(index), "Store Camera Bookmark " + std::to_string(index),
            "Camera", {ShortcutContext::Camera, ShortcutContext::Viewport});
        add("camera.load_bookmark_" + std::to_string(index), "Recall Camera Bookmark " + std::to_string(index),
            "Camera", {ShortcutContext::Camera, ShortcutContext::Viewport});
        add("camera.blend_bookmark_" + std::to_string(index), "Blend to Camera Bookmark " + std::to_string(index),
            "Camera", {ShortcutContext::Camera, ShortcutContext::Viewport});
    }

    add("play.play", "Play in Editor", "Play", {ShortcutContext::Global});
    add("play.simulate", "Simulate", "Play", {ShortcutContext::Global});
    add("play.stop", "Stop Play Session", "Play", {ShortcutContext::PlayMode});
    add("play.pause", "Pause Simulation", "Play", {ShortcutContext::PlayMode});
    add("play.step", "Advance One Simulation Frame", "Play", {ShortcutContext::PlayMode});
    add("play.eject", "Eject or Possess", "Play", {ShortcutContext::PlayMode});
    add("play.keep_changes", "Keep Runtime Changes", "Play", {ShortcutContext::PlayMode});
    add("play.release_cursor", "Release or Capture Cursor", "Play", {ShortcutContext::PlayMode});

    const auto timeline = [&](std::string id, std::string label) {
        add(std::move(id), std::move(label), "Sequencer", {ShortcutContext::Sequencer});
    };
    timeline("sequencer.play_pause", "Play or Pause");
    timeline("sequencer.play_forward", "Play Forward");
    timeline("sequencer.return_start", "Return to Playback Start");
    timeline("sequencer.step_previous", "Step Previous Frame");
    timeline("sequencer.step_next", "Step Next Frame");
    timeline("sequencer.previous_key", "Previous Key");
    timeline("sequencer.next_key", "Next Key");
    timeline("sequencer.set_in", "Set In Point");
    timeline("sequencer.set_out", "Set Out Point");
    timeline("sequencer.marker", "Add or Remove Marker");
    timeline("sequencer.key_selected", "Key Selected Property");
    timeline("sequencer.key_armed", "Key Armed Camera Properties");
    timeline("sequencer.split", "Split Shot at Playhead");
    timeline("sequencer.trim_left", "Trim Shot Left to Playhead");
    timeline("sequencer.trim_right", "Trim Shot Right to Playhead");
    timeline("sequencer.shuttle_reverse", "Reverse Shuttle");
    timeline("sequencer.shuttle_pause", "Pause Shuttle");
    timeline("sequencer.shuttle_forward", "Forward Shuttle");
    timeline("sequencer.loop", "Toggle Sequence Loop");

    const auto audio = [&](std::string id, std::string label) {
        add(std::move(id), std::move(label), "Audio Editor", {ShortcutContext::AudioEditor});
    };
    audio("audio.transport_play_pause", "Play or Pause");
    audio("audio.transport_reverse", "Reverse Shuttle");
    audio("audio.transport_stop", "Stop Shuttle");
    audio("audio.transport_forward", "Forward Shuttle");
    audio("audio.record", "Record");
    audio("audio.punch_record", "Punch Record");
    audio("audio.record_bus", "Record Selected Bus or Stem");
    audio("audio.set_in", "Set Selection In");
    audio("audio.set_out", "Set Selection Out");
    audio("audio.marker", "Add Marker");
    audio("audio.loop", "Toggle Loop");
    audio("audio.split", "Split Clip at Playhead");
    audio("audio.trim_left", "Trim Clip Left to Playhead");
    audio("audio.trim_right", "Trim Clip Right to Playhead");
    audio("audio.toggle_snap", "Toggle Audio Snapping");
    audio("audio.crossfade_tool", "Crossfade Tool");
    audio("audio.gain_envelope", "Toggle Clip Gain Envelope");
    audio("audio.automation", "Toggle Automation Lanes");
    audio("audio.bounce_selection", "Bounce Selected Clips");
    audio("audio.bounce_session", "Bounce Session");

    const auto voxel = [&](std::string id, std::string label) {
        add(std::move(id), std::move(label), "Voxel Editor", {ShortcutContext::VoxelEditor});
    };
    voxel("voxel.brush", "Voxel Add Brush");
    voxel("voxel.erase", "Voxel Erase Brush");
    voxel("voxel.toggle_operation", "Toggle Add or Subtract");
    voxel("voxel.fill", "Flood Fill");
    voxel("voxel.line", "Line Tool");
    voxel("voxel.volume", "Volume Tool");
    voxel("voxel.carve", "Carve Tool");
    voxel("voxel.paint_material", "Paint Material");
    voxel("voxel.sample_material", "Sample Voxel Material");
    voxel("voxel.brush_increase", "Increase Brush Radius");
    voxel("voxel.brush_decrease", "Decrease Brush Radius");
    voxel("voxel.commit", "Commit Staged Voxel Operation");
    voxel("voxel.rebuild_brickmaps", "Rebuild Affected Brickmaps");
    voxel("voxel.rebuild_collision", "Rebuild Collision Proxies");

    const auto polygon = [&](std::string id, std::string label) {
        add(std::move(id), std::move(label), "Polygon Editor", {ShortcutContext::PolygonEditor});
    };
    polygon("polygon.vertex_mode", "Vertex Selection Mode");
    polygon("polygon.edge_mode", "Edge Selection Mode");
    polygon("polygon.face_mode", "Face Selection Mode");
    polygon("polygon.object_mode", "Object Selection Mode");
    polygon("polygon.component_toggle", "Toggle Object or Component Editing");
    polygon("polygon.bevel", "Bevel");
    polygon("polygon.extrude", "Extrude");
    polygon("polygon.inset", "Inset");
    polygon("polygon.uv_tools", "UV Tools");
    polygon("polygon.recalculate_normals", "Recalculate Normals");
    polygon("polygon.flip_normals", "Flip Normals");

    const auto material = [&](std::string id, std::string label) {
        add(std::move(id), std::move(label), "Material Editor", {ShortcutContext::MaterialEditor});
    };
    material("material.assign", "Assign or Open Material");
    material("material.link", "Link Materials or Settings");
    material("material.open_layers", "Open Material Layers");
    material("material.create_instance", "Create Material Instance");
    material("material.create_master", "Create Master Material");
    material("material.open_globals", "Open Material Parameter Collection");
    material("material.preview", "Preview Material");
    material("material.pin_preview", "Pin Material Preview");
    material("material.apply", "Compile and Apply Material");

    add("asset.navigate_back", "Navigate Back", "Asset Browser", {ShortcutContext::AssetBrowser});
    add("asset.navigate_forward", "Navigate Forward", "Asset Browser", {ShortcutContext::AssetBrowser});
    add("modal.accept", "Accept Modal", "Modal", {ShortcutContext::ModalDialog});
    add("modal.cancel", "Cancel Modal", "Modal", {ShortcutContext::ModalDialog});
    add("text.cancel", "Cancel Text Edit", "Text", {ShortcutContext::TextField});
    add("text.commit", "Commit Text Edit", "Text", {ShortcutContext::TextField});
}

void EditorShortcutRegistry::install_builtin_profiles() {
    profiles_.clear();
    builtinDefaults_.clear();
    const auto ensure = [&](std::string_view profile) -> ProfileBindings& {
        return profiles_[std::string(profile)];
    };
    const auto bind = [&](std::string_view profile, std::string_view action, ShortcutContext context,
                          ShortcutGesture primary, std::optional<ShortcutGesture> secondary = std::nullopt) {
        ProfileBindings& profileBindings = ensure(profile);
        profileBindings[{std::string(action), context}] = {std::move(primary), std::move(secondary)};
    };

    constexpr std::string_view dve = "DVE Default";
    bind(dve, "file.new_scene", ShortcutContext::Global, keyboard_shortcut("n", true));
    bind(dve, "file.open_scene", ShortcutContext::Global, keyboard_shortcut("o", true));
    bind(dve, "file.save", ShortcutContext::Global, keyboard_shortcut("s", true));
    bind(dve, "file.save_as", ShortcutContext::Global, keyboard_shortcut("s", true, true));
    bind(dve, "edit.undo", ShortcutContext::Global, keyboard_shortcut("z", true));
    bind(dve, "edit.redo", ShortcutContext::Global, keyboard_shortcut("y", true), keyboard_shortcut("z", true, true));
    bind(dve, "edit.cut", ShortcutContext::Global, keyboard_shortcut("x", true));
    bind(dve, "edit.copy", ShortcutContext::Global, keyboard_shortcut("c", true));
    bind(dve, "edit.paste", ShortcutContext::Global, keyboard_shortcut("v", true));
    bind(dve, "edit.duplicate", ShortcutContext::Global, keyboard_shortcut("d", true));
    bind(dve, "edit.delete", ShortcutContext::Global, keyboard_shortcut("delete"));
    bind(dve, "edit.rename", ShortcutContext::Global, keyboard_shortcut("f2"));
    bind(dve, "edit.select_all", ShortcutContext::Global, keyboard_shortcut("a", true));
    bind(dve, "help.command_palette", ShortcutContext::Global, keyboard_shortcut("p", true, true), keyboard_shortcut("k", true));
    bind(dve, "edit.preferences", ShortcutContext::Global, keyboard_shortcut(",", true));
    bind(dve, "help.docs", ShortcutContext::Global, keyboard_shortcut("f1"));
    bind(dve, "render.diagnostics3d", ShortcutContext::Global, keyboard_shortcut("f11"));
    bind(dve, "build.package", ShortcutContext::Global, keyboard_shortcut("b", true, true));
    bind(dve, "build.validate", ShortcutContext::Global, keyboard_shortcut("f6"));
    bind(dve, "physics.play", ShortcutContext::Global, keyboard_shortcut("p", false, false, true));
    bind(dve, "physics.simulate", ShortcutContext::Global, keyboard_shortcut("s", false, false, true));
    bind(dve, "create.empty", ShortcutContext::Global, keyboard_shortcut("e", true, true));
    bind(dve, "create.voxel", ShortcutContext::Global, keyboard_shortcut("v", true, true));
    bind(dve, "create.polygon", ShortcutContext::Global, keyboard_shortcut("m", true, true));
    bind(dve, "file.exit", ShortcutContext::Global, keyboard_shortcut("q", true));
    bind(dve, "window.toggle_hierarchy", ShortcutContext::Global, keyboard_shortcut("1", true));
    bind(dve, "window.toggle_inspector", ShortcutContext::Global, keyboard_shortcut("2", true));
    bind(dve, "window.toggle_assets", ShortcutContext::Global, keyboard_shortcut("3", true));
    bind(dve, "window.toggle_synth", ShortcutContext::Global, keyboard_shortcut("4", true));
    bind(dve, "window.toggle_audio", ShortcutContext::Global, keyboard_shortcut("5", true));
    bind(dve, "window.toggle_audio_event", ShortcutContext::Global, keyboard_shortcut("6", true));
    bind(dve, "window.toggle_ai_assistant", ShortcutContext::Global, keyboard_shortcut("7", true));
    bind(dve, "window.toggle_control_rig", ShortcutContext::Global, keyboard_shortcut("8", true));
    bind(dve, "window.toggle_chiptune", ShortcutContext::Global, keyboard_shortcut("9", true));

    bind(dve, "transform.select", ShortcutContext::Viewport, keyboard_shortcut("q"));
    bind(dve, "transform.translate", ShortcutContext::Viewport, keyboard_shortcut("w"));
    bind(dve, "transform.rotate", ShortcutContext::Viewport, keyboard_shortcut("e"));
    bind(dve, "transform.scale", ShortcutContext::Viewport, keyboard_shortcut("r"));
    bind(dve, "transform.universal", ShortcutContext::Viewport, keyboard_shortcut("t"));
    bind(dve, "transform.space", ShortcutContext::Viewport, keyboard_shortcut("space"));
    bind(dve, "view.frame", ShortcutContext::Viewport, keyboard_shortcut("f"),
         mouse_shortcut("mouse1", false, false, false, ShortcutActivation::DoubleClick));
    bind(dve, "view.follow_selection", ShortcutContext::Viewport, keyboard_shortcut("f", false, true));
    bind(dve, "view.frame_all", ShortcutContext::Viewport, keyboard_shortcut("home"));
    bind(dve, "view.drop_to_surface", ShortcutContext::Viewport, keyboard_shortcut("end"));
    bind(dve, "view.decrease_snap", ShortcutContext::Viewport, keyboard_shortcut("["));
    bind(dve, "view.increase_snap", ShortcutContext::Viewport, keyboard_shortcut("]"));
    bind(dve, "view.decrease_angle_snap", ShortcutContext::Viewport, keyboard_shortcut("[", false, true));
    bind(dve, "view.increase_angle_snap", ShortcutContext::Viewport, keyboard_shortcut("]", false, true));
    bind(dve, "view.game_view", ShortcutContext::Viewport, keyboard_shortcut("g"));
    bind(dve, "view.realtime", ShortcutContext::Viewport, keyboard_shortcut("r", true));
    bind(dve, "view.immersive", ShortcutContext::Viewport, keyboard_shortcut("f11"));
    bind(dve, "view.lit", ShortcutContext::Viewport, keyboard_shortcut("1", false, false, true));
    bind(dve, "view.unlit", ShortcutContext::Viewport, keyboard_shortcut("2", false, false, true));
    bind(dve, "view.wireframe", ShortcutContext::Viewport, keyboard_shortcut("3", false, false, true));
    bind(dve, "view.material_ids", ShortcutContext::Viewport, keyboard_shortcut("4", false, false, true));
    bind(dve, "view.normals", ShortcutContext::Viewport, keyboard_shortcut("5", false, false, true));
    bind(dve, "view.collision", ShortcutContext::Viewport, keyboard_shortcut("6", false, false, true));
    bind(dve, "view.voxel_debug", ShortcutContext::Viewport, keyboard_shortcut("7", false, false, true));
    bind(dve, "view.lighting_only", ShortcutContext::Viewport, keyboard_shortcut("8", false, false, true));
    bind(dve, "view.overdraw", ShortcutContext::Viewport, keyboard_shortcut("9", false, false, true));
    bind(dve, "viewport.look", ShortcutContext::Viewport, mouse_shortcut("mouse2", false, false, false, ShortcutActivation::Hold));
    bind(dve, "viewport.orbit", ShortcutContext::Viewport, mouse_shortcut("mouse1", false, false, true, ShortcutActivation::Hold));
    bind(dve, "viewport.pan", ShortcutContext::Viewport, mouse_shortcut("mouse3", false, false, false, ShortcutActivation::Hold));
    bind(dve, "viewport.dolly", ShortcutContext::Viewport, mouse_shortcut("mouse2", false, false, true, ShortcutActivation::Hold));
    bind(dve, "view.zoom_in", ShortcutContext::Viewport, wheel_shortcut(true));
    bind(dve, "view.zoom_out", ShortcutContext::Viewport, wheel_shortcut(false));
    bind(dve, "view.previous_camera", ShortcutContext::Viewport, mouse_shortcut("mouse4"));
    bind(dve, "view.next_camera", ShortcutContext::Viewport, mouse_shortcut("mouse4", false, true));

    bind(dve, "camera.fly_forward", ShortcutContext::FlyNavigation, keyboard_shortcut("w", false, false, false, ShortcutActivation::Hold));
    bind(dve, "camera.fly_backward", ShortcutContext::FlyNavigation, keyboard_shortcut("s", false, false, false, ShortcutActivation::Hold));
    bind(dve, "camera.fly_left", ShortcutContext::FlyNavigation, keyboard_shortcut("a", false, false, false, ShortcutActivation::Hold));
    bind(dve, "camera.fly_right", ShortcutContext::FlyNavigation, keyboard_shortcut("d", false, false, false, ShortcutActivation::Hold));
    bind(dve, "camera.fly_down", ShortcutContext::FlyNavigation, keyboard_shortcut("q", false, false, false, ShortcutActivation::Hold));
    bind(dve, "camera.fly_up", ShortcutContext::FlyNavigation, keyboard_shortcut("e", false, false, false, ShortcutActivation::Hold));

    bind(dve, "camera.create_from_view", ShortcutContext::Camera, keyboard_shortcut("c", false, false, true));
    bind(dve, "camera.align_to_view", ShortcutContext::Camera, keyboard_shortcut("f", true, true));
    bind(dve, "camera.toggle_preview", ShortcutContext::Camera, keyboard_shortcut("c"));
    bind(dve, "camera.toggle_safe_frames", ShortcutContext::Camera, keyboard_shortcut("c", false, true));
    bind(dve, "camera.lock", ShortcutContext::Camera, keyboard_shortcut("l", true));
    bind(dve, "camera.capture_screenshot", ShortcutContext::Camera, keyboard_shortcut("f9"));
    bind(dve, "camera.capture_selected", ShortcutContext::Camera, keyboard_shortcut("f9", false, true));
    for (int index = 1; index <= 9; ++index) {
        bind(dve, "camera.save_bookmark_" + std::to_string(index), ShortcutContext::Camera,
             keyboard_shortcut(std::to_string(index), true, false, true));
        bind(dve, "camera.load_bookmark_" + std::to_string(index), ShortcutContext::Camera,
             keyboard_shortcut(std::to_string(index)));
        bind(dve, "camera.blend_bookmark_" + std::to_string(index), ShortcutContext::Camera,
             keyboard_shortcut(std::to_string(index), false, true));
    }

    bind(dve, "play.stop", ShortcutContext::PlayMode, keyboard_shortcut("escape"));
    bind(dve, "play.pause", ShortcutContext::PlayMode, keyboard_shortcut("pause"));
    bind(dve, "play.step", ShortcutContext::PlayMode, keyboard_shortcut("."));
    bind(dve, "play.eject", ShortcutContext::PlayMode, keyboard_shortcut("f8"));
    bind(dve, "play.release_cursor", ShortcutContext::PlayMode, keyboard_shortcut("f1", false, true));

    bind(dve, "sequencer.play_pause", ShortcutContext::Sequencer, keyboard_shortcut("space"));
    bind(dve, "sequencer.play_forward", ShortcutContext::Sequencer, keyboard_shortcut("down"));
    bind(dve, "sequencer.return_start", ShortcutContext::Sequencer, keyboard_shortcut("up"));
    bind(dve, "sequencer.step_previous", ShortcutContext::Sequencer, keyboard_shortcut("left"));
    bind(dve, "sequencer.step_next", ShortcutContext::Sequencer, keyboard_shortcut("right"));
    bind(dve, "sequencer.previous_key", ShortcutContext::Sequencer, keyboard_shortcut("left", false, true));
    bind(dve, "sequencer.next_key", ShortcutContext::Sequencer, keyboard_shortcut("right", false, true));
    bind(dve, "sequencer.set_in", ShortcutContext::Sequencer, keyboard_shortcut("i"));
    bind(dve, "sequencer.set_out", ShortcutContext::Sequencer, keyboard_shortcut("o"));
    bind(dve, "sequencer.marker", ShortcutContext::Sequencer, keyboard_shortcut("m"));
    bind(dve, "sequencer.key_selected", ShortcutContext::Sequencer, keyboard_shortcut("enter"));
    bind(dve, "sequencer.key_armed", ShortcutContext::Sequencer, keyboard_shortcut("k", true));
    bind(dve, "sequencer.split", ShortcutContext::Sequencer, keyboard_shortcut("/", true));
    bind(dve, "sequencer.trim_left", ShortcutContext::Sequencer, keyboard_shortcut(",", true));
    bind(dve, "sequencer.trim_right", ShortcutContext::Sequencer, keyboard_shortcut(".", true));
    bind(dve, "sequencer.shuttle_reverse", ShortcutContext::Sequencer, keyboard_shortcut("j"));
    bind(dve, "sequencer.shuttle_pause", ShortcutContext::Sequencer, keyboard_shortcut("k"));
    bind(dve, "sequencer.shuttle_forward", ShortcutContext::Sequencer, keyboard_shortcut("l"));
    bind(dve, "sequencer.loop", ShortcutContext::Sequencer, keyboard_shortcut("l", true));

    bind(dve, "audio.transport_play_pause", ShortcutContext::AudioEditor, keyboard_shortcut("space"));
    bind(dve, "audio.transport_reverse", ShortcutContext::AudioEditor, keyboard_shortcut("j"));
    bind(dve, "audio.transport_stop", ShortcutContext::AudioEditor, keyboard_shortcut("k"));
    bind(dve, "audio.transport_forward", ShortcutContext::AudioEditor, keyboard_shortcut("l"));
    bind(dve, "audio.record", ShortcutContext::AudioEditor, keyboard_shortcut("r"));
    bind(dve, "audio.punch_record", ShortcutContext::AudioEditor, keyboard_shortcut("r", false, true));
    bind(dve, "audio.record_bus", ShortcutContext::AudioEditor, keyboard_shortcut("r", true));
    bind(dve, "audio.set_in", ShortcutContext::AudioEditor, keyboard_shortcut("i"));
    bind(dve, "audio.set_out", ShortcutContext::AudioEditor, keyboard_shortcut("o"));
    bind(dve, "audio.marker", ShortcutContext::AudioEditor, keyboard_shortcut("m"));
    bind(dve, "audio.loop", ShortcutContext::AudioEditor, keyboard_shortcut("l", true));
    bind(dve, "audio.split", ShortcutContext::AudioEditor, keyboard_shortcut("s"));
    bind(dve, "audio.trim_left", ShortcutContext::AudioEditor, keyboard_shortcut(",", true));
    bind(dve, "audio.trim_right", ShortcutContext::AudioEditor, keyboard_shortcut(".", true));
    bind(dve, "audio.toggle_snap", ShortcutContext::AudioEditor, keyboard_shortcut("n"));
    bind(dve, "audio.crossfade_tool", ShortcutContext::AudioEditor, keyboard_shortcut("x"));
    bind(dve, "audio.gain_envelope", ShortcutContext::AudioEditor, keyboard_shortcut("g"));
    bind(dve, "audio.automation", ShortcutContext::AudioEditor, keyboard_shortcut("a"));
    bind(dve, "audio.bounce_selection", ShortcutContext::AudioEditor, keyboard_shortcut("b", true));
    bind(dve, "audio.bounce_session", ShortcutContext::AudioEditor, keyboard_shortcut("b", true, true));

    bind(dve, "voxel.brush", ShortcutContext::VoxelEditor, keyboard_shortcut("b"));
    bind(dve, "voxel.erase", ShortcutContext::VoxelEditor, keyboard_shortcut("b", false, true));
    bind(dve, "voxel.toggle_operation", ShortcutContext::VoxelEditor, keyboard_shortcut("x"));
    bind(dve, "voxel.fill", ShortcutContext::VoxelEditor, keyboard_shortcut("f"));
    bind(dve, "voxel.line", ShortcutContext::VoxelEditor, keyboard_shortcut("l"));
    bind(dve, "voxel.volume", ShortcutContext::VoxelEditor, keyboard_shortcut("v"));
    bind(dve, "voxel.carve", ShortcutContext::VoxelEditor, keyboard_shortcut("c"));
    bind(dve, "voxel.paint_material", ShortcutContext::VoxelEditor, keyboard_shortcut("p"));
    bind(dve, "voxel.sample_material", ShortcutContext::VoxelEditor, mouse_shortcut("mouse4"));
    bind(dve, "voxel.brush_decrease", ShortcutContext::VoxelEditor, keyboard_shortcut("["), wheel_shortcut(false, true));
    bind(dve, "voxel.brush_increase", ShortcutContext::VoxelEditor, keyboard_shortcut("]"), wheel_shortcut(true, true));
    bind(dve, "voxel.commit", ShortcutContext::VoxelEditor, keyboard_shortcut("enter", true));
    bind(dve, "voxel.rebuild_brickmaps", ShortcutContext::VoxelEditor, keyboard_shortcut("r", true, true));
    bind(dve, "voxel.rebuild_collision", ShortcutContext::VoxelEditor, keyboard_shortcut("r", true, false, true));

    bind(dve, "polygon.vertex_mode", ShortcutContext::PolygonEditor, keyboard_shortcut("1"));
    bind(dve, "polygon.edge_mode", ShortcutContext::PolygonEditor, keyboard_shortcut("2"));
    bind(dve, "polygon.face_mode", ShortcutContext::PolygonEditor, keyboard_shortcut("3"));
    bind(dve, "polygon.object_mode", ShortcutContext::PolygonEditor, keyboard_shortcut("4"));
    bind(dve, "polygon.component_toggle", ShortcutContext::PolygonEditor, keyboard_shortcut("tab"));
    bind(dve, "polygon.bevel", ShortcutContext::PolygonEditor, keyboard_shortcut("b"));
    bind(dve, "polygon.extrude", ShortcutContext::PolygonEditor, keyboard_shortcut("e"));
    bind(dve, "polygon.inset", ShortcutContext::PolygonEditor, keyboard_shortcut("i"));
    bind(dve, "polygon.uv_tools", ShortcutContext::PolygonEditor, keyboard_shortcut("u"));
    bind(dve, "polygon.recalculate_normals", ShortcutContext::PolygonEditor, keyboard_shortcut("n"));
    bind(dve, "polygon.flip_normals", ShortcutContext::PolygonEditor, keyboard_shortcut("n", false, true));

    bind(dve, "material.assign", ShortcutContext::MaterialEditor, keyboard_shortcut("m"));
    bind(dve, "material.link", ShortcutContext::MaterialEditor, keyboard_shortcut("l", true));
    bind(dve, "material.open_layers", ShortcutContext::MaterialEditor, keyboard_shortcut("l", true, true));
    bind(dve, "material.create_instance", ShortcutContext::MaterialEditor, keyboard_shortcut("m", true));
    bind(dve, "material.create_master", ShortcutContext::MaterialEditor, keyboard_shortcut("m", true, true));
    bind(dve, "material.open_globals", ShortcutContext::MaterialEditor, keyboard_shortcut("m", false, false, true));
    bind(dve, "material.preview", ShortcutContext::MaterialEditor, keyboard_shortcut("p"));
    bind(dve, "material.pin_preview", ShortcutContext::MaterialEditor, keyboard_shortcut("p", false, true));
    bind(dve, "material.apply", ShortcutContext::MaterialEditor, keyboard_shortcut("enter", true));

    bind(dve, "asset.navigate_back", ShortcutContext::AssetBrowser, mouse_shortcut("mouse4"));
    bind(dve, "modal.accept", ShortcutContext::ModalDialog, keyboard_shortcut("enter"));
    bind(dve, "modal.cancel", ShortcutContext::ModalDialog, keyboard_shortcut("escape"));
    bind(dve, "text.commit", ShortcutContext::TextField, keyboard_shortcut("enter"));
    bind(dve, "text.cancel", ShortcutContext::TextField, keyboard_shortcut("escape"));

    profiles_["Unity Familiar"] = profiles_[std::string(dve)];
    profiles_["Unreal Familiar"] = profiles_[std::string(dve)];
    profiles_["Accessibility One-Handed"] = profiles_[std::string(dve)];
    profiles_["Blank Custom"] = {};

    // Unity play-mode convention and Scene-view tool conventions.
    profiles_["Unity Familiar"][{"physics.play", ShortcutContext::Global}] =
        {keyboard_shortcut("p", true), std::nullopt};
    profiles_["Unity Familiar"][{"transform.select", ShortcutContext::Viewport}] =
        {keyboard_shortcut("q"), std::nullopt};
    profiles_["Unity Familiar"][{"view.follow_selection", ShortcutContext::Viewport}] =
        {keyboard_shortcut("f", false, true), std::nullopt};

    // Unreal keeps Alt+P/Alt+S and Q/W/E/R. DVE Default already follows this layout.
    profiles_["Unreal Familiar"] = profiles_[std::string(dve)];

    // One-handed profile keeps destructive operations modified and moves primary tools around
    // the left hand; Mouse4 remains non-destructive navigation/sample behavior.
    ProfileBindings& accessible = profiles_["Accessibility One-Handed"];
    accessible[{"transform.translate", ShortcutContext::Viewport}] = {keyboard_shortcut("a", true, false, true), std::nullopt};
    accessible[{"transform.rotate", ShortcutContext::Viewport}] = {keyboard_shortcut("s", true, false, true), std::nullopt};
    accessible[{"transform.scale", ShortcutContext::Viewport}] = {keyboard_shortcut("d", true, false, true), std::nullopt};
    accessible[{"edit.delete", ShortcutContext::Global}] = {keyboard_shortcut("delete", true), std::nullopt};
    accessible[{"physics.play", ShortcutContext::Global}] = {keyboard_shortcut("p", true, false, true), std::nullopt};

    builtinDefaults_ = profiles_;
    activeProfile_ = std::string(dve);
}

} // namespace dve::editor
