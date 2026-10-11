#include "dve/editor_workspace.hpp"
#include "dve/editor_ui_zoom.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <iomanip>
#include <sstream>
#include <utility>

namespace dve::editor {
namespace {

template <class T>
bool parse_number(std::string_view text, T& value) {
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

std::map<std::string, std::string, std::less<>> parse_lines(std::string_view text) {
    std::map<std::string, std::string, std::less<>> result;
    std::istringstream stream{std::string(text)};
    std::string line;
    while (std::getline(stream, line)) {
        const std::size_t equals = line.find('=');
        if (equals != std::string::npos) result[line.substr(0, equals)] = line.substr(equals + 1);
    }
    return result;
}

} // namespace

bool EditorPreferences::validate(std::string* error) const {
    auto fail = [&](std::string message) { if (error) *error = std::move(message); return false; };
    if (!std::isfinite(uiScale) || uiScale < kUiZoomMin || uiScale > kUiZoomMax) return fail("UI zoom must be between 1.0 and 2.0");
    if (!std::isfinite(cameraSpeed) || cameraSpeed <= 0.0F || cameraSpeed > 1000.0F) return fail("camera speed is invalid");
    if (!std::isfinite(mouseSensitivity) || mouseSensitivity <= 0.0F || mouseSensitivity > 20.0F) return fail("mouse sensitivity is invalid");
    if (autosaveMinutes == 0 || autosaveMinutes > 120) return fail("autosave interval must be 1 to 120 minutes");
    if (!std::isfinite(translateSnapMeters) || translateSnapMeters <= 0.0F || translateSnapMeters > 100.0F)
        return fail("translate snap must be between 0 and 100 meters");
    if (!std::isfinite(rotateSnapDegrees) || rotateSnapDegrees <= 0.0F || rotateSnapDegrees > 180.0F)
        return fail("rotate snap must be between 0 and 180 degrees");
    if (!std::isfinite(scaleSnapStep) || scaleSnapStep <= 0.0F || scaleSnapStep > 10.0F)
        return fail("scale snap must be between 0 and 10");
    return true;
}

std::string EditorPreferences::serialize() const {
    std::ostringstream out;
    out << "DVE_EDITOR_PREFERENCES=1\n"
        << "uiScale=" << uiScale << "\n"
        << "cameraSpeed=" << cameraSpeed << "\n"
        << "mouseSensitivity=" << mouseSensitivity << "\n"
        << "autosaveMinutes=" << autosaveMinutes << "\n"
        << "translateSnapMeters=" << translateSnapMeters << "\n"
        << "rotateSnapDegrees=" << rotateSnapDegrees << "\n"
        << "scaleSnapStep=" << scaleSnapStep << "\n"
        << "translateSnapEnabled=" << translateSnapEnabled << "\n"
        << "rotateSnapEnabled=" << rotateSnapEnabled << "\n"
        << "scaleSnapEnabled=" << scaleSnapEnabled << "\n"
        << "absoluteGridSnap=" << absoluteGridSnap << "\n"
        << "highContrast=" << highContrast << "\n"
        << "reducedMotion=" << reducedMotion << "\n"
        << "colorBlindSafeDiagnostics=" << colorBlindSafeDiagnostics << "\n"
        << "confirmDestructiveActions=" << confirmDestructiveActions << "\n";
    return out.str();
}

std::optional<EditorPreferences> EditorPreferences::parse(std::string_view text, std::string* error) {
    EditorPreferences result;
    const auto values = parse_lines(text);
    auto fail = [&](std::string message) -> std::optional<EditorPreferences> { if (error) *error = std::move(message); return std::nullopt; };
    if (!values.contains("DVE_EDITOR_PREFERENCES")) return fail("missing preferences version");
    auto readFloat = [&](std::string_view key, float& target) {
        const auto it = values.find(std::string(key));
        return it == values.end() || parse_number<float>(it->second, target);
    };
    auto readUInt = [&](std::string_view key, std::uint32_t& target) {
        const auto it = values.find(std::string(key));
        return it == values.end() || parse_number<std::uint32_t>(it->second, target);
    };
    auto readBool = [&](std::string_view key, bool& target) {
        const auto it = values.find(std::string(key));
        if (it == values.end()) return true;
        if (it->second == "1" || it->second == "true") { target = true; return true; }
        if (it->second == "0" || it->second == "false") { target = false; return true; }
        return false;
    };
    if (!readFloat("uiScale", result.uiScale) || !readFloat("cameraSpeed", result.cameraSpeed) ||
        !readFloat("mouseSensitivity", result.mouseSensitivity) || !readUInt("autosaveMinutes", result.autosaveMinutes) ||
        !readFloat("translateSnapMeters", result.translateSnapMeters) ||
        !readFloat("rotateSnapDegrees", result.rotateSnapDegrees) ||
        !readFloat("scaleSnapStep", result.scaleSnapStep) ||
        !readBool("translateSnapEnabled", result.translateSnapEnabled) ||
        !readBool("rotateSnapEnabled", result.rotateSnapEnabled) ||
        !readBool("scaleSnapEnabled", result.scaleSnapEnabled) ||
        !readBool("absoluteGridSnap", result.absoluteGridSnap) ||
        !readBool("highContrast", result.highContrast) || !readBool("reducedMotion", result.reducedMotion) ||
        !readBool("colorBlindSafeDiagnostics", result.colorBlindSafeDiagnostics) ||
        !readBool("confirmDestructiveActions", result.confirmDestructiveActions)) return fail("invalid preferences value");
    // Files written before UI zoom used a free 0.75-3.0 scale; snap instead of rejecting them.
    result.uiScale = snap_ui_zoom(result.uiScale);
    std::string validation;
    if (!result.validate(&validation)) return fail(validation);
    return result;
}

bool EditorMenuRegistry::add(MenuAction action, std::string* error) {
    if (action.id.empty() || action.menu.empty() || action.label.empty()) {
        if (error) *error = "menu action requires id, menu, and label";
        return false;
    }
    if (find_for_update(action.id)) { if (error) *error = "duplicate menu action id"; return false; }
    if (!action.shortcut.empty()) {
        for (const MenuAction& existing : actions_) {
            if (existing.shortcut == action.shortcut) {
                if (error) *error = "shortcut already assigned to " + existing.id;
                return false;
            }
        }
    }
    const std::size_t index = actions_.size();
    actions_.push_back(std::move(action));
    try {
        actionIndices_.emplace(actions_.back().id, index);
    } catch (...) {
        actions_.pop_back();
        throw;
    }
    searchFieldsDirty_ = menusDirty_ = true;
    searchResultsValid_ = false;
    ++revision_;
    return true;
}
const MenuAction* EditorMenuRegistry::find(std::string_view id) const noexcept {
    if (!mutableActionExposed_) {
        const auto it = actionIndices_.find(id);
        return it == actionIndices_.end() ? nullptr : &actions_[it->second];
    }
    const auto it = std::find_if(actions_.begin(), actions_.end(), [&](const MenuAction& action) { return action.id == id; });
    return it == actions_.end() ? nullptr : &*it;
}
MenuAction* EditorMenuRegistry::find(std::string_view id) noexcept {
    MenuAction* action = find_for_update(id);
    if (action) mutableActionExposed_ = true;
    return action;
}
MenuAction* EditorMenuRegistry::find_for_update(std::string_view id) noexcept {
    return const_cast<MenuAction*>(std::as_const(*this).find(id));
}
bool EditorMenuRegistry::set_enabled(std::string_view id, bool enabled, std::string reason) noexcept {
    if (MenuAction* action = find_for_update(id)) {
        if (action->enabled != enabled) searchResultsValid_ = false;
        std::string newReason = enabled ? std::string{} : std::move(reason);
        if (action->enabled != enabled || action->disabledReason != newReason) ++revision_;
        action->enabled = enabled;
        action->disabledReason = std::move(newReason);
        return true;
    }
    return false;
}
bool EditorMenuRegistry::set_checked(std::string_view id, bool checked) noexcept {
    MenuAction* action = find_for_update(id);
    if (!action) return false;
    if (!action->radioGroup.empty() && checked) {
        for (MenuAction& candidate : actions_)
            if (candidate.radioGroup == action->radioGroup) candidate.checked = false;
    }
    action->checked = checked;
    ++revision_;
    return true;
}
bool EditorMenuRegistry::set_shortcut(std::string_view id, std::string shortcut) noexcept {
    if (MenuAction* action = find_for_update(id)) {
        if (action->shortcut != shortcut) {
            action->shortcut = std::move(shortcut);
            searchFieldsDirty_ = true;
            searchResultsValid_ = false;
            ++revision_;
        }
        return true;
    }
    return false;
}
std::vector<std::size_t> EditorMenuRegistry::ordered_menu_indices(
    std::string_view menuName, bool includeAdvanced) const {
    std::map<std::string, std::size_t, std::less<>> sections;
    std::vector<std::size_t> indices;
    for (std::size_t index = 0; index < actions_.size(); ++index) {
        const MenuAction& action = actions_[index];
        if (action.menu != menuName || action.visibility == MenuVisibility::PaletteOnly ||
            (action.visibility == MenuVisibility::Advanced && !includeAdvanced)) continue;
        sections.try_emplace(action.section, sections.size());
        indices.push_back(index);
    }
    std::sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
        const auto sectionA = sections.at(actions_[a].section);
        const auto sectionB = sections.at(actions_[b].section);
        if (sectionA != sectionB) return sectionA < sectionB;
        if (actions_[a].order != actions_[b].order) return actions_[a].order < actions_[b].order;
        return a < b;
    });
    return indices;
}

void EditorMenuRegistry::rebuild_menu_indices() const {
    menuIndices_.clear();
    for (const MenuAction& action : actions_) menuIndices_.try_emplace(action.menu);
    for (auto& [menuName, modes] : menuIndices_) {
        modes[0] = ordered_menu_indices(menuName, false);
        modes[1] = ordered_menu_indices(menuName, true);
    }
    menusDirty_ = false;
}

std::vector<MenuAction> EditorMenuRegistry::menu(std::string_view menuName, bool includeAdvanced) const {
    std::vector<MenuAction> result;
    const auto copy_actions = [&](const auto& indices) {
        result.reserve(indices.size());
        for (std::size_t index : indices) result.push_back(actions_[index]);
    };
    if (mutableActionExposed_) {
        // A retained pointer can change membership/order at any time. Rebuild only
        // this menu and mode, rather than every menu in both modes on each read.
        copy_actions(ordered_menu_indices(menuName, includeAdvanced));
    } else {
        if (menusDirty_) rebuild_menu_indices();
        const auto found = menuIndices_.find(menuName);
        if (found != menuIndices_.end()) copy_actions(found->second[includeAdvanced ? 1U : 0U]);
    }
    return result;
}

void EditorMenuRegistry::rebuild_search_fields() const {
    const auto lower = [](std::string_view value) {
        std::string result(value);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return result;
    };
    searchFields_.clear();
    searchFields_.reserve(actions_.size());
    for (const MenuAction& action : actions_) {
        auto& fields = searchFields_.emplace_back();
        fields.reserve(6U + action.keywords.size());
        for (std::string_view value : {std::string_view(action.label), std::string_view(action.id),
                std::string_view(action.menu), std::string_view(action.section),
                std::string_view(action.description), std::string_view(action.shortcut)})
            fields.push_back(lower(value));
        for (const auto& keyword : action.keywords) fields.push_back(lower(keyword));
    }
    searchFieldsDirty_ = false;
    searchResultsValid_ = false;
}

std::vector<MenuAction> EditorMenuRegistry::search(std::string_view query, std::size_t limit) const {
    const auto lower = [](std::string_view value) {
        std::string result(value);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return result;
    };
    const auto split = [&](std::string_view value) {
        std::vector<std::string> result;
        std::istringstream stream(lower(value));
        std::string token;
        while (stream >> token) result.push_back(std::move(token));
        return result;
    };
    const auto field_score = [](std::string_view field, std::string_view token) -> int {
        if (field == token) return 0;
        if (field.starts_with(token)) return 8;
        if (const std::size_t position = field.find(token); position != std::string_view::npos)
            return 20 + static_cast<int>(std::min<std::size_t>(position, 40U));
        std::size_t cursor = 0U;
        int gap = 0;
        for (const char c : field) {
            if (cursor < token.size() && c == token[cursor]) ++cursor;
            else if (cursor > 0U && cursor < token.size()) ++gap;
        }
        return cursor == token.size() ? 60 + std::min(gap, 60) : -1;
    };

    if (limit == 0) return {};
    if (searchFieldsDirty_ || mutableActionExposed_) rebuild_search_fields();
    if (!searchResultsValid_ || query != searchQuery_) {
        const std::vector<std::string> tokens = split(query);
        struct Match { int score{}; std::size_t order{}; };
        std::vector<Match> matches;
        matches.reserve(actions_.size());
        for (std::size_t index = 0; index < actions_.size(); ++index) {
            const MenuAction& action = actions_[index];
            const auto& ownedFields = searchFields_[index];
            int score = tokens.empty() ? 100 : 0;
            bool matched = true;
            for (const std::string& token : tokens) {
                int best = -1;
                for (std::size_t fieldIndex = 0; fieldIndex < ownedFields.size(); ++fieldIndex) {
                    int candidate = field_score(ownedFields[fieldIndex], token);
                    if (candidate >= 0) candidate += static_cast<int>(fieldIndex) * 3;
                    if (candidate >= 0 && (best < 0 || candidate < best)) best = candidate;
                }
                if (best < 0) { matched = false; break; }
                score += best;
            }
            if (!matched) continue;
            if (!action.enabled) score += 500;
            if (action.visibility == MenuVisibility::Advanced) score += 12;
            if (action.visibility == MenuVisibility::PaletteOnly) score += 18;
            matches.push_back({score, index});
        }
        std::stable_sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
            if (a.score != b.score) return a.score < b.score;
            return a.order < b.order;
        });
        searchResults_.clear();
        searchResults_.reserve(matches.size());
        for (const Match& match : matches) searchResults_.push_back(match.order);
        searchQuery_.assign(query);
        searchResultsValid_ = true;
    }
    std::vector<MenuAction> result;
    result.reserve(std::min(limit, searchResults_.size()));
    for (std::size_t index = 0; index < searchResults_.size() && index < limit; ++index)
        result.push_back(actions_[searchResults_[index]]);
    return result;
}

bool EditorMenuUserState::validate(std::string* error) const {
    const auto fail = [&](std::string message) { if (error) *error = std::move(message); return false; };
    if (favoriteActionIds.size() > 128U || recentActionIds.size() > 64U)
        return fail("menu state contains too many command identifiers");
    const auto validIds = [](const std::vector<std::string>& values) {
        std::set<std::string, std::less<>> unique;
        for (const std::string& value : values) {
            if (value.empty() || value.size() > 256U || !unique.insert(value).second) return false;
        }
        return true;
    };
    if (!validIds(favoriteActionIds)) return fail("favorite command identifiers are invalid or duplicated");
    if (!validIds(recentActionIds)) return fail("recent command identifiers are invalid or duplicated");
    return true;
}

std::string EditorMenuUserState::serialize() const {
    std::ostringstream out;
    out << "DVE_MENU_STATE 1\n";
    out << "advanced " << (showAdvancedCommands ? 1 : 0) << '\n';
    out << "favorites " << favoriteActionIds.size() << '\n';
    for (const std::string& id : favoriteActionIds) out << std::quoted(id) << '\n';
    out << "recent " << recentActionIds.size() << '\n';
    for (const std::string& id : recentActionIds) out << std::quoted(id) << '\n';
    return out.str();
}

std::optional<EditorMenuUserState> EditorMenuUserState::parse(std::string_view text, std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<EditorMenuUserState> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    std::istringstream in{std::string(text)};
    std::string magic;
    int version = 0;
    if (!(in >> magic >> version) || magic != "DVE_MENU_STATE" || version != 1)
        return fail("menu state header is invalid");
    EditorMenuUserState result;
    std::string key;
    int advanced = 0;
    if (!(in >> key >> advanced) || key != "advanced" || (advanced != 0 && advanced != 1))
        return fail("menu advanced-state record is invalid");
    result.showAdvancedCommands = advanced != 0;
    std::size_t count = 0U;
    if (!(in >> key >> count) || key != "favorites" || count > 128U)
        return fail("menu favorites record is invalid");
    for (std::size_t index = 0; index < count; ++index) {
        std::string id;
        if (!(in >> std::quoted(id))) return fail("menu favorite identifier is invalid");
        result.favoriteActionIds.push_back(std::move(id));
    }
    if (!(in >> key >> count) || key != "recent" || count > 64U)
        return fail("menu recent record is invalid");
    for (std::size_t index = 0; index < count; ++index) {
        std::string id;
        if (!(in >> std::quoted(id))) return fail("menu recent identifier is invalid");
        result.recentActionIds.push_back(std::move(id));
    }
    std::string validation;
    if (!result.validate(&validation)) return fail(validation);
    return result;
}

bool EditorMenuUserState::save(const std::filesystem::path& path, std::string* error) const {
    std::string validation;
    if (!validate(&validation)) { if (error) *error = validation; return false; }
    std::error_code directoryError;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), directoryError);
    if (directoryError) { if (error) *error = "could not create menu-state directory"; return false; }
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) { if (error) *error = "could not open temporary menu-state file"; return false; }
        out << serialize();
        if (!out) { if (error) *error = "could not write menu-state file"; return false; }
    }
    std::error_code renameError;
    std::filesystem::rename(temporary, path, renameError);
    if (renameError) {
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
        renameError.clear();
        std::filesystem::rename(temporary, path, renameError);
    }
    if (renameError) { if (error) *error = "could not replace menu-state file"; return false; }
    return true;
}

std::optional<EditorMenuUserState> EditorMenuUserState::load(
    const std::filesystem::path& path, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { if (error) *error = "could not open menu-state file"; return std::nullopt; }
    std::ostringstream text;
    text << in.rdbuf();
    if (!in.good() && !in.eof()) { if (error) *error = "could not read menu-state file"; return std::nullopt; }
    return parse(text.str(), error);
}

EditorMenuRegistry EditorMenuRegistry::make_default() {
    EditorMenuRegistry result;
    const std::vector<MenuAction> actions{
        {"file.new_project","File","New Project","Ctrl+Shift+N"}, {"file.open_project","File","Open Project","Ctrl+Shift+O"},
        {"file.new_scene","File","New Scene","Ctrl+N"}, {"file.open_scene","File","Open Scene","Ctrl+O"},
        {"file.save","File","Save","Ctrl+S"}, {"file.save_as","File","Save As","Ctrl+Shift+S"},
        {"file.import","File","Import Model","Ctrl+I"}, {"file.export_print_stl","File","Export for Printing (STL)",""},
        {"file.exit","File","Exit","Alt+F4"},
        {"edit.undo","Edit","Undo","Ctrl+Z"}, {"edit.redo","Edit","Redo","Ctrl+Y"},
        {"edit.cut","Edit","Cut","Ctrl+X"}, {"edit.copy","Edit","Copy","Ctrl+C"},
        {"edit.paste","Edit","Paste","Ctrl+V"}, {"edit.delete","Edit","Delete","Delete"},
        {"edit.select_all","Edit","Select All","Ctrl+A"}, {"transform.select","Edit","Select Tool",""},
        {"transform.translate","Edit","Move Tool",""}, {"transform.rotate","Edit","Rotate Tool",""},
        {"transform.scale","Edit","Scale Tool",""}, {"transform.universal","Edit","Universal Transform",""},
        {"transform.space","Edit","Toggle Local / World",""},
        {"transform.pivot_bounds","Edit","Pivot at Selection Center",""},
        {"transform.pivot_active","Edit","Pivot at Active Object",""},
        {"transform.pivot_origin","Edit","Pivot at World Origin",""},
        {"transform.pivot_cursor","Edit","Pivot at Cursor Hit",""},
        {"transform.place_surface","Edit","Place on Surface",""},
        {"transform.place_voxel_corner","Edit","Snap to Voxel Corner",""},
        {"transform.place_voxel_edge","Edit","Snap to Voxel Edge",""},
        {"transform.place_voxel_face","Edit","Snap to Voxel Face",""},
        {"transform.place_bounds_vertex","Edit","Snap to Bounds Vertex",""},
        {"transform.place_collision_vertex","Edit","Snap to Collision Box Vertex",""},
        {"transform.align_surface","Edit","Align Up to Surface Normal",""},
        {"transform.surface_offset_more","Edit","Increase Surface Offset",""},
        {"transform.surface_offset_less","Edit","Decrease Surface Offset",""},
        {"transform.scale_double","Edit","Double Voxel Object Size",""},
        {"transform.scale_half","Edit","Halve Voxel Object Size",""},
        {"transform.scale_voxel_size_up","Edit","Scale Voxel Size x2",""},
        {"transform.scale_voxel_size_down","Edit","Scale Voxel Size x0.5",""},
        {"edit.duplicate","Edit","Duplicate","Ctrl+D"}, {"edit.rename","Edit","Rename","F2"},
        {"edit.group","Edit","Group Selected","Ctrl+G"}, {"edit.ungroup","Edit","Ungroup","Ctrl+Shift+G"},
        {"edit.preferences","Edit","Editor Preferences","Ctrl+,"},
        {"edit.project_settings","Edit","Project Settings",""},
        {"window.toggle_hierarchy","Window","Scene Hierarchy","Ctrl+1"},
        {"window.toggle_inspector","Window","Inspector","Ctrl+2"},
        {"window.toggle_assets","Window","Assets","Ctrl+3"},
        {"asset.refresh","Window","Refresh Asset Index","Ctrl+Alt+R"},
        {"asset.rename","Window","Rename Selected Asset",""},
        {"asset.thumbnail","Window","Regenerate Selected Thumbnail",""},
        {"asset.filter_stale","Window","Show Only Stale/Broken Assets",""},
        {"asset.open","Window","Open Selected Asset","Enter"},
        {"window.toggle_synth","Window","Synthesizer","Ctrl+4"},
        {"window.toggle_audio","Window","Audio Mixer","Ctrl+5"},
        {"window.toggle_audio_event","Window","Audio Event Graph","Ctrl+6"},
        {"window.toggle_ai_assistant","Window","AI Assistant","Ctrl+7"},
        {"window.toggle_control_rig","Window","Control Rig Editor","Ctrl+8"},
        {"window.toggle_chiptune","Window","Chiptune Tracker","Ctrl+9"},
        {"window.toggle_live_mcp","Window","Live MCP Host",""},
        {"window.settings","Window","Settings",""},
        {"window.camera_preview","Window","Camera Preview",""},
        {"window.cinematic_camera","Window","Cinematic Camera Inspector",""},
        {"window.camera_sequencer","Window","Camera Sequencer",""},
        {"window.camera_lut_browser","Window","Camera LUT Browser",""},
        {"window.camera_diagnostics","Window","Camera Diagnostics",""},
        {"window.gabor_inspector","Window","Gabor Volume Inspector",""},
        {"view.frame","View","Frame Selection","F"}, {"view.grid","View","Grid","G"},
        {"view.isolate_selection","View","Isolate Selection",""},
        {"view.top","View","Top View","Numpad7"}, {"view.front","View","Front View","Numpad1"},
        {"view.side","View","Side View","Numpad3"}, {"view.perspective","View","Perspective View","Numpad0"},
        {"view.increase_snap","View","Increase Move Snap","]"}, {"view.decrease_snap","View","Decrease Move Snap","["},
        {"view.toggle_move_snap","View","Snap Movement",""},
        {"view.toggle_angle_snap","View","Snap Rotation",""},
        {"view.toggle_scale_snap","View","Snap Scale",""},
        {"view.toggle_absolute_grid","View","Absolute Grid Alignment",""},
        {"view.increase_scale_snap","View","Increase Scale Snap",""},
        {"view.decrease_scale_snap","View","Decrease Scale Snap",""},
        {"view.increase_angle_snap","View","Increase Angle Snap","Shift+]"},
        {"view.decrease_angle_snap","View","Decrease Angle Snap","Shift+["},
        {"view.collision","View","Collision Shapes","Ctrl+Alt+C"}, {"view.anchors","View","Anchors","Ctrl+Alt+A"},
        {"view.bounds","View","Object Bounds",""}, {"view.xray","View","X-Ray Selection",""},
        {"view.statistics","View","Viewport Statistics",""}, {"view.safe_frames","View","Safe Frames",""},
        {"camera.inspector","Camera","Cinematic Camera Inspector",""},
        {"camera.sequencer","Camera","Camera Sequencer",""},
        {"camera.pilot_selected","Camera","Pilot Selected Camera",""},
        {"camera.lock_viewport","Camera","Lock Viewport to Camera",""},
        {"camera.preview_effects","Camera","Preview Cinematic Effects",""},
        {"camera.overlay_focus","Camera","Focus-Plane Overlay",""},
        {"camera.overlay_split_diopter","Camera","Split-Diopter Overlay",""},
        {"camera.overlay_safe_frames","Camera","Safe-Frame Overlay",""},
        {"camera.overlay_aspect_mattes","Camera","Aspect-Matte Overlay",""},
        {"camera.overlay_motion_vectors","Camera","Motion-Vector Overlay",""},
        {"camera.overlay_exposure","Camera","Exposure Preview",""},
        {"camera.compare_graded","Camera","Compare Graded / Ungraded",""},
        {"camera.reset_preview","Camera","Reset Camera Preview Overrides",""},
        {"view.advanced_menus","View","Show Advanced Menu Commands",""},
        {"view.ui_zoom_in","View","Zoom UI In","Ctrl+="}, {"view.ui_zoom_out","View","Zoom UI Out","Ctrl+-"},
        {"view.ui_zoom_reset","View","Reset UI Zoom (100%)","Ctrl+0"},
        {"view.keyboard_keys_25","View","Piano: 25 Keys",""}, {"view.keyboard_keys_37","View","Piano: 37 Keys",""},
        {"view.keyboard_keys_49","View","Piano: 49 Keys",""}, {"view.keyboard_keys_61","View","Piano: 61 Keys",""},
        {"view.keyboard_keys_76","View","Piano: 76 Keys",""}, {"view.keyboard_keys_88","View","Piano: 88 Keys",""},
        {"camera.mode_free","Camera","Free Fly",""}, {"camera.mode_orbit","Camera","Orbit",""},
        {"camera.mode_follow","Camera","Follow",""}, {"camera.mode_third_person","Camera","Third Person",""},
        {"camera.mode_first_person","Camera","First Person",""}, {"camera.mode_cinematic","Camera","Cinematic",""},
        {"camera.projection_perspective","Camera","Perspective",""},
        {"camera.projection_orthographic","Camera","Orthographic",""},
        {"camera.physical_lens","Camera","Physical Lens",""},
        {"camera.collision","Camera","Collision Avoidance",""},
        {"camera.preview_selected","Camera","Preview Selected Cameras",""},
        {"camera.show_frustum","Camera","Show Camera Frustum",""},
        {"camera.preview_shakes","Camera","Preview Camera Shakes",""},
        {"camera.save_bookmark_1","Camera","Save Bookmark 1","Ctrl+Alt+1"},
        {"camera.load_bookmark_1","Camera","Load Bookmark 1","Alt+1"},
        {"camera.save_bookmark_2","Camera","Save Bookmark 2","Ctrl+Alt+2"},
        {"camera.load_bookmark_2","Camera","Load Bookmark 2","Alt+2"},
        {"camera.reset","Camera","Reset Camera",""},
        {"camera.settings","Camera","Camera Settings",""},
        {"camera.scope_instance","Camera","Edit Camera Instance",""},
        {"camera.scope_shot","Camera","Edit Shot Override",""},
        {"camera.scope_project","Camera","Edit Project Camera Defaults",""},
        {"camera.scope_preview","Camera","Edit Viewport Preview Only",""},
        {"camera.preset_neutral","Camera","Preset: Neutral",""},
        {"camera.preset_academy","Camera","Preset: Academy Classic",""},
        {"camera.preset_imax143","Camera","Preset: IMAX 1.43",""},
        {"camera.preset_imax190","Camera","Preset: IMAX 1.90",""},
        {"camera.preset_scope239","Camera","Preset: Scope 2.39",""},
        {"camera.preset_anamorphic","Camera","Preset: Vintage Anamorphic",""},
        {"camera.preset_fisheye","Camera","Preset: Fisheye Action",""},
        {"camera.preset_split_diopter","Camera","Preset: Split Diopter",""},
        {"camera.preset_bleach_bypass","Camera","Preset: Bleach Bypass",""},
        {"camera.preset_seventies","Camera","Preset: Seventies Film",""},
        {"camera.filmback_super16","Camera","Filmback: Super 16",""},
        {"camera.filmback_super35","Camera","Filmback: Super 35",""},
        {"camera.filmback_full_frame","Camera","Filmback: Full Frame 35",""},
        {"camera.filmback_anamorphic35","Camera","Filmback: Anamorphic 35",""},
        {"camera.filmback_imax15","Camera","Filmback: IMAX 15-perf",""},
        {"camera.filmback_imax_digital","Camera","Filmback: IMAX Digital",""},
        {"camera.copy_profile","Camera","Copy Cinematic Profile",""},
        {"camera.paste_profile","Camera","Paste Cinematic Profile",""},
        {"camera.clear_effects","Camera","Clear Cinematic Effects",""},
        {"camera.keyframe_profile","Camera","Keyframe Complete Cinematic Profile",""},
        {"create.empty","Create","Empty Object","Ctrl+Shift+E"}, {"create.voxel","Create","Voxel Object","Ctrl+Shift+V"},
        {"create.polygon","Create","Polygon Mesh","Ctrl+Shift+M"}, {"create.camera","Create","Camera Rig",""},
        {"create.text3d","Create","3D Text","Ctrl+Shift+T"},
        {"create.gabor_empty","Create","Volume: Empty Gabor Volume",""},
        {"create.gabor_import","Create","Volume: Import Gabor Field",""},
        {"create.prefab_from_selection","Create","Prefab from Selection","Ctrl+Shift+P"},
        {"create.scatter","Create","Scatter Objects",""},
        {"scatter.commit","Create","Commit Scatter","Enter"},
        {"scatter.cancel","Create","Cancel Scatter","Esc"},
        {"asset.instantiate_prefab","Create","Instantiate Selected Prefab",""},
        {"text3d.edit_selected","Create","Edit Selected 3D Text",""},
        {"text3d.commit","Create","Commit 3D Text Changes",""},
        {"text3d.cancel","Create","Cancel 3D Text Changes",""},
        {"voxel.add","Voxel","Add Brush","3"}, {"voxel.remove","Voxel","Remove Brush","4"},
        {"voxel.paint","Voxel","Paint Material","5"}, {"voxel.box","Voxel","Box Tool","6"},
        {"voxel.beam","Voxel","Beam Tool","7"}, {"voxel.anchor","Voxel","Anchor Brush","8"},
        {"voxel.settings","Voxel","Voxel Settings",""},
        {"voxel.join","Voxel","Join Objects",""},
        {"voxel.join_commit","Voxel","Commit Join",""},
        {"voxel.join_cancel","Voxel","Cancel Join",""},
        {"voxel.separate_islands","Voxel","Separate Islands",""},
        {"voxel.slice","Voxel","Slice Voxel Object",""},
        {"voxel.slice_commit","Voxel","Commit Slice",""},
        {"voxel.slice_cancel","Voxel","Cancel Slice",""},
        {"voxel.boolean_union","Voxel","Boolean Union",""},
        {"voxel.boolean_difference","Voxel","Boolean Difference",""},
        {"voxel.boolean_intersection","Voxel","Boolean Intersection",""},
        {"voxel.boolean_commit","Voxel","Commit Boolean",""},
        {"voxel.boolean_cancel","Voxel","Cancel Boolean",""},
        {"voxel.boolean_swap","Voxel","Swap Boolean Target",""},
        {"voxel.boolean_operands_hide","Voxel","Boolean Operands: Hide",""},
        {"voxel.boolean_operands_delete","Voxel","Boolean Operands: Delete",""},
        {"voxel.boolean_operands_keep","Voxel","Boolean Operands: Keep",""},
        {"polygon.import","Polygon","Import Polygon Model",""}, {"polygon.lod","Polygon","LOD Settings",""},
        {"polygon.collision","Polygon","Collision Settings",""}, {"polygon.streaming","Polygon","Streaming Settings",""},
        {"material.library","Materials","Material Library",""}, {"material.globals","Materials","Global Parameters",""},
        {"material.layers","Materials","Material Layers",""}, {"material.rendering","Materials","Shading Models",""},
        {"render.diagnostics3d","Rendering","3D Rendering Diagnostics","F11"},
        {"render.voxel_material_policy","Rendering","Voxel Material Policy",""},
        {"render.gabor.enabled","Rendering","Enable Gabor Fields",""},
        {"render.gabor.mode_absorption","Rendering","Gabor: Absorption Preview",""},
        {"render.gabor.mode_emission_absorption","Rendering","Gabor: Emission-Absorption",""},
        {"render.gabor.mode_scattering","Rendering","Gabor: Scattering [Experimental]",""},
        {"render.gabor.quality_low","Rendering","Gabor Quality: Low",""},
        {"render.gabor.quality_medium","Rendering","Gabor Quality: Medium",""},
        {"render.gabor.quality_high","Rendering","Gabor Quality: High",""},
        {"render.gabor.quality_cinematic","Rendering","Gabor Quality: Cinematic",""},
        {"render.gabor.continuous_lod","Rendering","Gabor Continuous LOD",""},
        {"render.gabor.temporal","Rendering","Gabor Temporal Accumulation",""},
        {"render.gabor.shadows","Rendering","Gabor Volume Shadows",""},
        {"render.gabor.settings","Rendering","Gabor Volume Settings",""},
        {"sprite.play_level","Sprite","Play Original Sprite Level","F9"},
        {"sprite.tile_world","Sprite","Tile World Editor","F8"},
        {"sprite.diagnostics","Sprite","Sprite Diagnostics","F10"},
        {"sprite.graph","Sprite","Animation State Graph",""},
        {"sprite.pixel_art","Sprite","Pixel Art Studio","F7"},
        {"sprite.rig2d","Sprite","Multi-Part Sprite Rig","Shift+F7"},
        {"sprite.editor","Sprite","Sprite Editor",""},
        {"sprite.save","Sprite","Save Sprite Asset",""},
        {"sprite.reimport","Sprite","Reimport Sprite Source",""},
        {"sprite.slice_grid","Sprite","Slice Sprite Grid",""},
        {"sprite.repack","Sprite","Repack Sprite Atlas",""},
        {"audio.workstation","Audio","Music and Sound Editor",""}, {"audio.mixer","Audio","Mixer",""},
        {"audio.synth","Audio","Synthesizer",""}, {"audio.tracker","Audio","Chiptune Tracker",""},
        {"audio.events","Audio","Interactive Audio",""},
        {"audio.import","Audio","Import Audio",""}, {"audio.settings","Audio","Audio Settings",""},
        {"physics.simulate","Physics","Simulate","F6"}, {"physics.play","Physics","Play","F5"},
        {"physics.stop","Physics","Stop","Shift+F5"}, {"physics.settings","Physics","Physics Settings",""},
        {"build.cook","Build","Cook Scene","Ctrl+B"}, {"build.package","Build","Package Game","Ctrl+Shift+B"},
        {"build.settings","Build","Build Settings",""}, {"build.validate","Build","Validate Project",""},
        {"help.docs","Help","Documentation","F1"},
        {"help.command_palette","Help","Command Center","Ctrl+K"}, {"help.shortcuts","Help","Keyboard Shortcuts",""},
        {"help.reset_command_history","Help","Reset Command Favorites and History",""},
        {"help.about","Help","About",""}
    };
    for (const auto& action : actions) {
        const bool added = result.add(action);
        (void)added;
    }
    const auto configure = [&](std::string_view id, std::string section, std::int32_t order,
                               bool checkable = false, std::string radioGroup = {}) {
        if (MenuAction* action = result.find_for_update(id)) {
            action->section = std::move(section);
            action->order = order;
            action->checkable = checkable;
            action->radioGroup = std::move(radioGroup);
        }
    };
    configure("edit.preferences", "Settings", 90);
    configure("edit.project_settings", "Settings", 91);
    configure("window.toggle_ai_assistant", "AI", 70, true);
    configure("window.toggle_live_mcp", "AI", 71, true);
    configure("window.settings", "Settings", 90);
    configure("window.camera_preview", "Camera", 70, true);
    configure("render.diagnostics3d", "Diagnostics", 5, true);
    configure("render.voxel_material_policy", "Voxel Materials", 10);
    configure("render.gabor.enabled", "Gabor Volumes", 10, true);
    for (std::string_view id : {"render.gabor.mode_absorption","render.gabor.mode_emission_absorption","render.gabor.mode_scattering"})
        configure(id, "Gabor Mode", 20, true, "gabor_mode");
    for (std::string_view id : {"render.gabor.quality_low","render.gabor.quality_medium","render.gabor.quality_high","render.gabor.quality_cinematic"})
        configure(id, "Gabor Quality", 30, true, "gabor_quality");
    configure("render.gabor.continuous_lod", "Gabor Features", 40, true);
    configure("render.gabor.temporal", "Gabor Features", 41, true);
    configure("render.gabor.shadows", "Gabor Features", 42, true);
    configure("render.gabor.settings", "Gabor Volumes", 90);
    for (std::string_view id : {"view.grid","view.collision","view.anchors","view.bounds","view.xray","view.statistics","view.safe_frames"})
        configure(id, "Overlays", 50, true);
    configure("view.isolate_selection", "Visibility", 45, true);
    for (std::string_view id : {"view.toggle_move_snap", "view.toggle_angle_snap",
                                "view.toggle_scale_snap", "view.toggle_absolute_grid"})
        configure(id, "Snapping", 30, true);
    configure("view.advanced_menus", "Interface", 90, true);
    configure("transform.align_surface", "Placement", 20, true);
    configure("view.ui_zoom_in", "Interface", 91);
    configure("view.ui_zoom_out", "Interface", 92);
    configure("view.ui_zoom_reset", "Interface", 93);
    for (std::string_view id : {"view.keyboard_keys_25","view.keyboard_keys_37","view.keyboard_keys_49",
                                "view.keyboard_keys_61","view.keyboard_keys_76","view.keyboard_keys_88"})
        configure(id, "Piano Keyboard", 95, true, "view.keyboard_keys");
    configure("view.top", "Projection", 20, true, "view.projection");
    configure("view.front", "Projection", 21, true, "view.projection");
    configure("view.side", "Projection", 22, true, "view.projection");
    configure("view.perspective", "Projection", 23, true, "view.projection");
    for (std::string_view id : {"camera.mode_free","camera.mode_orbit","camera.mode_follow","camera.mode_third_person","camera.mode_first_person","camera.mode_cinematic"})
        configure(id, "Rig Mode", 10, true, "camera.mode");
    configure("camera.projection_perspective", "Lens", 30, true, "camera.projection");
    configure("camera.projection_orthographic", "Lens", 31, true, "camera.projection");
    configure("camera.physical_lens", "Lens", 32, true);
    configure("camera.collision", "Behavior", 40, true);
    configure("camera.preview_selected", "Preview", 50, true);
    configure("camera.show_frustum", "Preview", 51, true);
    configure("camera.preview_shakes", "Preview", 52, true);
    configure("camera.save_bookmark_1", "Bookmarks", 60);
    configure("camera.load_bookmark_1", "Bookmarks", 61);
    configure("camera.save_bookmark_2", "Bookmarks", 62);
    configure("camera.load_bookmark_2", "Bookmarks", 63);
    configure("camera.settings", "Settings", 90);
    configure("voxel.settings", "Settings", 90);
    configure("voxel.join", "Voxel Join and Separate", 56);
    configure("voxel.join_commit", "Voxel Join and Separate", 57);
    configure("voxel.join_cancel", "Voxel Join and Separate", 58);
    configure("voxel.separate_islands", "Voxel Join and Separate", 59);
    configure("voxel.slice", "Voxel Slice", 66);
    configure("voxel.slice_commit", "Voxel Slice", 67);
    configure("voxel.slice_cancel", "Voxel Slice", 68);
    configure("voxel.boolean_union", "Voxel Boolean", 60);
    configure("voxel.boolean_difference", "Voxel Boolean", 61);
    configure("voxel.boolean_intersection", "Voxel Boolean", 62);
    configure("voxel.boolean_commit", "Voxel Boolean", 63);
    configure("voxel.boolean_cancel", "Voxel Boolean", 64);
    configure("voxel.boolean_swap", "Voxel Boolean", 64);
    for (std::string_view id : {"voxel.boolean_operands_hide", "voxel.boolean_operands_delete", "voxel.boolean_operands_keep"})
        configure(id, "Voxel Boolean", 65, true, "voxel.boolean_operands");
    configure("physics.settings", "Settings", 90);
    configure("build.settings", "Settings", 90);
    configure("audio.settings", "Settings", 90);
    configure("sprite.play_level", "Sprite Authoring", 5);
    configure("sprite.tile_world", "Sprite Authoring", 6);
    configure("sprite.diagnostics", "Sprite Authoring", 7);
    configure("sprite.graph", "Sprite Authoring", 8);
    configure("sprite.pixel_art", "Sprite Authoring", 9, true);
    configure("sprite.rig2d", "Sprite Authoring", 10, true);
    configure("sprite.editor", "Sprite Authoring", 11);
    configure("sprite.save", "Sprite Authoring", 20);
    configure("sprite.reimport", "Sprite Authoring", 30);
    configure("sprite.slice_grid", "Sprite Authoring", 40);
    configure("sprite.repack", "Sprite Authoring", 50);

    // Keep the application menu bar compact. Domain-specific editors live under Tools,
    // while camera controls are part of View. Stable action ids preserve shortcuts,
    // automation, saved layouts, and MCP clients.
    for (MenuAction& action : result.actions_) {
        const std::string originalMenu = action.menu;
        if (originalMenu == "Camera") {
            action.menu = "View";
            if (action.section.empty()) action.section = "Camera";
        } else if (originalMenu == "Voxel" || originalMenu == "Polygon" ||
                   originalMenu == "Materials" || originalMenu == "Rendering" ||
                   originalMenu == "Sprite" || originalMenu == "Audio" || originalMenu == "Physics") {
            action.menu = "Tools";
            if (action.section.empty()) action.section = originalMenu;
        }
        if (action.section.empty()) {
            if (action.menu == "File") {
                action.section = action.id.starts_with("file.new") || action.id.starts_with("file.open")
                    ? "Project and Scene" : action.id.starts_with("file.save") ? "Save" : "Application";
            } else if (action.menu == "Edit") {
                action.section = action.id.starts_with("transform.") ? "Transform"
                    : action.id.starts_with("edit.preferences") || action.id.starts_with("edit.project")
                        ? "Settings" : "Editing";
            } else if (action.menu == "Create") action.section = "Objects";
            else if (action.menu == "View") action.section = "Viewport";
            else if (action.menu == "Build") action.section = "Project";
            else if (action.menu == "Window") action.section = "Panels";
            else if (action.menu == "Help") action.section = "Help";
        }
        action.keywords.push_back(originalMenu);
        action.keywords.push_back(action.section);
        action.description = action.label + " in " + action.section + ".";
    }
    const auto advanced = [&](std::string_view id) {
        if (MenuAction* action = result.find_for_update(id)) action->visibility = MenuVisibility::Advanced;
    };
    const auto paletteOnly = [&](std::string_view id) {
        if (MenuAction* action = result.find_for_update(id)) action->visibility = MenuVisibility::PaletteOnly;
    };
    for (std::string_view id : {
            "asset.thumbnail", "asset.filter_stale", "transform.scale_double", "transform.scale_half",
            "view.xray", "camera.physical_lens", "camera.collision", "camera.preview_shakes",
            "camera.save_bookmark_1", "camera.load_bookmark_1", "camera.save_bookmark_2",
            "camera.load_bookmark_2", "polygon.lod", "polygon.collision", "polygon.streaming",
            "material.globals", "material.layers", "material.rendering", "render.gabor.mode_absorption",
            "render.gabor.mode_emission_absorption", "render.gabor.mode_scattering",
            "render.gabor.quality_low", "render.gabor.quality_medium", "render.gabor.quality_high",
            "render.gabor.quality_cinematic", "render.gabor.continuous_lod", "render.gabor.temporal",
            "render.gabor.shadows", "sprite.slice_grid", "sprite.repack", "audio.settings",
            "physics.settings", "build.settings"}) advanced(id);
    for (std::string_view id : {"text3d.commit", "text3d.cancel", "voxel.boolean_commit", "voxel.boolean_cancel", "voxel.boolean_swap",
            "camera.scope_instance", "camera.scope_shot", "camera.scope_project", "camera.scope_preview",
            "camera.preset_neutral", "camera.preset_academy", "camera.preset_imax143",
            "camera.preset_imax190", "camera.preset_scope239", "camera.preset_anamorphic",
            "camera.preset_fisheye", "camera.preset_split_diopter", "camera.preset_bleach_bypass",
            "camera.preset_seventies", "camera.filmback_super16", "camera.filmback_super35",
            "camera.filmback_full_frame", "camera.filmback_anamorphic35", "camera.filmback_imax15",
            "camera.filmback_imax_digital", "camera.copy_profile", "camera.paste_profile",
            "camera.clear_effects", "camera.keyframe_profile"}) paletteOnly(id);
    const auto describe = [&](std::string_view id, std::string description, std::initializer_list<const char*> keywords) {
        if (MenuAction* action = result.find_for_update(id)) {
            action->description = std::move(description);
            for (const char* keyword : keywords) action->keywords.emplace_back(keyword);
        }
    };
    describe("transform.scale_voxel_size_up",
             "Double the voxel size of the selected voxel objects about the pivot; the voxel count is unchanged (no resampling). One undo step.",
             {"scale", "resize", "grow", "bigger", "voxel size", "keep voxels", "uniform"});
    describe("transform.scale_voxel_size_down",
             "Halve the voxel size of the selected voxel objects about the pivot; the voxel count is unchanged (no resampling). One undo step.",
             {"scale", "resize", "shrink", "smaller", "voxel size", "keep voxels", "uniform"});
    describe("create.scatter",
             "Preview copies of the selected objects (or the selected prefab) placed pseudo-randomly on the last selected voxel object; Enter commits as one undo step.",
             {"scatter", "foliage", "random", "distribute", "populate", "rocks", "trees", "organic", "place"});
    describe("scatter.commit", "Add the previewed scatter copies under one group as one undoable edit.", {"scatter", "apply"});
    describe("scatter.cancel", "Close the scatter preview without changing the scene.", {"scatter"});
    describe("voxel.slice", "Preview a cell-centre plane cut; keep front, back, or both as separate objects.", {"slice", "cut", "plane"});
    describe("voxel.join",
             "Preview merging the other selected voxel objects into the active one as one object (not a group); off-grid objects ask to resample or group instead.",
             {"join", "merge", "combine", "weld", "attach", "unite", "one object"});
    describe("voxel.join_commit", "Apply the open Join preview in one undo step.", {"join", "apply"});
    describe("voxel.join_cancel", "Close Join without changing the scene.", {"join", "escape"});
    describe("voxel.separate_islands",
             "Split each selected voxel object into its connected pieces; the largest piece stays, the others become sibling objects in place.",
             {"separate", "split", "islands", "pieces", "detach", "loose parts", "break apart"});
    describe("voxel.slice_commit", "Apply the open Slice preview in one undo step.", {"slice", "apply"});
    describe("voxel.slice_cancel", "Close Slice without changing the scene.", {"slice", "escape"});
    describe("voxel.boolean_union",
             "Preview adding the other selected voxel objects (B) to the active voxel object (A); Enter commits as one undo step.",
             {"csg", "boolean", "merge", "combine", "add", "join", "weld"});
    describe("voxel.boolean_difference",
             "Preview carving the other selected voxel objects (B) out of the active voxel object (A); Enter commits as one undo step.",
             {"csg", "boolean", "subtract", "subtraction", "minus", "carve", "cut"});
    describe("voxel.boolean_intersection",
             "Preview keeping only the part of the active voxel object (A) inside the other selected objects (B); Enter commits as one undo step.",
             {"csg", "boolean", "intersect", "common", "overlap", "and"});
    describe("voxel.boolean_commit", "Apply the open voxel Boolean preview as one undoable edit.", {"csg", "boolean", "apply"});
    describe("voxel.boolean_cancel", "Close the open voxel Boolean preview without changing the scene.", {"csg", "boolean", "escape"});
    describe("voxel.boolean_swap", "Make the first operand the Boolean target (A) and the target an operand (B).", {"csg", "boolean", "order", "reverse"});
    describe("voxel.boolean_operands_hide", "After a Boolean, hide the operand objects (default; show them again to re-run).", {"csg", "boolean", "output"});
    describe("voxel.boolean_operands_delete", "After a Boolean, delete the operand objects (undo restores them).", {"csg", "boolean", "output"});
    describe("voxel.boolean_operands_keep", "After a Boolean, leave the operand objects visible and unchanged.", {"csg", "boolean", "output"});
    for (std::string_view id : {"file.exit", "edit.delete", "help.reset_command_history"})
        if (MenuAction* action = result.find_for_update(id)) action->dangerous = true;
    if (MenuAction* palette = result.find_for_update("help.command_palette")) {
        palette->description = "Search commands, settings, panels, assets, scene objects, and documentation.";
        palette->keywords.insert(palette->keywords.end(), {"search", "quick actions", "options", "assets", "objects", "panels"});
    }
    if (MenuAction* preferences = result.find_for_update("edit.preferences"))
        preferences->description = "Open user-specific editor preferences.";
    if (MenuAction* project = result.find_for_update("edit.project_settings"))
        project->description = "Open settings stored with the current project.";

    if (MenuAction* cinematic = result.find_for_update("camera.inspector")) {
        cinematic->description = "Open the grouped physical-lens, bokeh, split-diopter, grading, film, framing, and accessibility inspector.";
        cinematic->keywords.insert(cinematic->keywords.end(), {"bokeh", "split diopter", "fisheye", "IMAX", "anamorphic", "color grading", "LUT", "film grain", "motion blur"});
    }
    if (MenuAction* sequencer = result.find_for_update("camera.sequencer")) {
        sequencer->description = "Open cinematic shot and complete-profile keyframe authoring.";
        sequencer->keywords.insert(sequencer->keywords.end(), {"timeline", "shot", "rack focus", "dolly zoom", "aspect ratio"});
    }
    if (MenuAction* advancedMenus = result.find_for_update("view.advanced_menus")) {
        advancedMenus->description = "Show low-frequency and expert commands in ordinary menus; all commands remain searchable.";
        advancedMenus->keywords.insert(advancedMenus->keywords.end(), {"expert", "compact menus", "hidden commands"});
    }
    if (MenuAction* reset = result.find_for_update("help.reset_command_history"))
        reset->description = "Clear locally persisted command favorites and recent-command history.";
    if (MenuAction* diagnostics = result.find_for_update("sprite.diagnostics"))
        diagnostics->description = "Open draw-call, overdraw, atlas, residency, sorting, and budget diagnostics.";
    if (MenuAction* diagnostics3d = result.find_for_update("render.diagnostics3d")) {
        diagnostics3d->description = "Open mesh, pipeline, residency, LOD, skinning, shadow, light, occlusion, and depth-complexity diagnostics.";
        diagnostics3d->keywords.insert(diagnostics3d->keywords.end(), {"profiler", "triangles", "vertices", "overdraw", "gpu", "lod"});
    }
    if (MenuAction* tileWorld = result.find_for_update("sprite.tile_world"))
        tileWorld->description = "Open the tileset, level, collision, prefab, validation, and play-test workspace.";
    return result;
}

void EditorProblemStore::add(EditorProblem problem) { problems_.push_back(std::move(problem)); }
void EditorProblemStore::clear_code(std::string_view code) {
    std::erase_if(problems_, [&](const EditorProblem& problem) { return problem.code == code; });
}
std::size_t EditorProblemStore::error_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(problems_.begin(), problems_.end(), [](const EditorProblem& problem) {
        return problem.severity == ProblemSeverity::Error;
    }));
}

void EditorLogStore::add(EditorLogLevel level, std::string text) {
    entries_.push_back({level, std::move(text)});
    while (entries_.size() > capacity_) entries_.pop_front();
}

EditorWorkspace::EditorWorkspace(EditorDocument document)
    : document_(std::move(document)), menus_(EditorMenuRegistry::make_default()),
      settings_(EditorSettingsRegistry::make_default()) {
    shortcuts_.install_builtin_commands();
    for (const MenuAction& action : menus_.actions()) {
        ShortcutCommandDefinition command;
        command.actionId = action.id;
        command.label = action.label;
        command.category = action.menu;
        command.contexts = {ShortcutContext::Global};
        if (action.id.starts_with("transform.") || action.id.starts_with("view."))
            command.contexts = {ShortcutContext::Viewport};
        else if (action.id.starts_with("camera."))
            command.contexts = {ShortcutContext::Camera, ShortcutContext::Viewport};
        else if (action.id.starts_with("voxel."))
            command.contexts = {ShortcutContext::VoxelEditor};
        else if (action.id.starts_with("polygon."))
            command.contexts = {ShortcutContext::PolygonEditor};
        else if (action.id.starts_with("material."))
            command.contexts = {ShortcutContext::MaterialEditor};
        else if (action.id.starts_with("sprite."))
            command.contexts = {ShortcutContext::Global};
        command.keywords = action.keywords;
        std::string ignored;
        (void)shortcuts_.add_command(std::move(command), &ignored);
    }
    shortcuts_.install_builtin_profiles();
    for (const MenuAction& action : menus_.actions())
        (void)menus_.set_shortcut(action.id, shortcuts_.display_binding(action.id));
    for (int value = static_cast<int>(PanelId::Project); value <= static_cast<int>(PanelId::Assistant); ++value)
        panels_[static_cast<PanelId>(value)] = true;
    synchronize_preferences_from_settings();
}

void EditorWorkspace::synchronize_preferences_from_settings() noexcept {
    const auto readFloat = [&](std::string_view id, float fallback) {
        const SettingValue value = settings_.value(id);
        if (const auto* number = std::get_if<double>(&value)) return static_cast<float>(*number);
        return fallback;
    };
    const auto readInteger = [&](std::string_view id, std::uint32_t fallback) {
        const SettingValue value = settings_.value(id);
        if (const auto* number = std::get_if<std::int64_t>(&value))
            return static_cast<std::uint32_t>(std::max<std::int64_t>(0, *number));
        return fallback;
    };
    const auto readBool = [&](std::string_view id, bool fallback) {
        const SettingValue value = settings_.value(id);
        if (const auto* boolean = std::get_if<bool>(&value)) return *boolean;
        return fallback;
    };
    preferences_.uiScale = snap_ui_zoom(readFloat(kUiZoomSettingId, preferences_.uiScale));
    preferences_.cameraSpeed = readFloat("camera.fly_speed", preferences_.cameraSpeed);
    preferences_.mouseSensitivity = readFloat("camera.mouse_sensitivity", preferences_.mouseSensitivity);
    preferences_.autosaveMinutes = readInteger("editor.autosave_minutes", preferences_.autosaveMinutes);
    preferences_.highContrast = readBool("accessibility.high_contrast", preferences_.highContrast);
    preferences_.reducedMotion = readBool("accessibility.reduced_motion", preferences_.reducedMotion);
    preferences_.confirmDestructiveActions = readBool("editor.confirm_destructive", preferences_.confirmDestructiveActions);
}
bool EditorWorkspace::set_mode(EditorMode mode, std::string* error) {
    if (mode == mode_) return true;
    if (mode_ == EditorMode::Edit && mode != EditorMode::Edit) {
        std::string validation;
        if (!document_.validate(&validation)) { if (error) *error = validation; return false; }
        preSimulationSnapshot_.emplace(clone_editor_document(document_));
        preSimulationSelection_ = selection_;
        preSimulationPrimarySelection_ = primarySelection_;
    } else if (mode == EditorMode::Edit && mode_ != EditorMode::Edit) {
        return restore_pre_simulation(error);
    }
    mode_ = mode;
    return true;
}
void EditorWorkspace::set_panel_visible(PanelId panel, bool visible) { panels_[panel] = visible; }
bool EditorWorkspace::panel_visible(PanelId panel) const {
    const auto it = panels_.find(panel);
    return it != panels_.end() && it->second;
}

void EditorWorkspace::select_object(std::optional<EditorObjectId> id) noexcept {
    selection_.clear();
    primarySelection_.reset();
    if (id && document_.find_object(*id)) {
        selection_.insert(*id);
        primarySelection_ = *id;
    }
}

void EditorWorkspace::add_to_selection(EditorObjectId id) noexcept {
    if (!document_.find_object(id)) return;
    selection_.insert(id);
    primarySelection_ = id;
}

void EditorWorkspace::toggle_selection(EditorObjectId id) noexcept {
    if (!document_.find_object(id)) return;
    if (selection_.contains(id)) {
        selection_.erase(id);
        if (primarySelection_ == id) {
            primarySelection_ = selection_.empty() ? std::nullopt : std::optional<EditorObjectId>(*selection_.rbegin());
        }
    } else {
        selection_.insert(id);
        primarySelection_ = id;
    }
}

void EditorWorkspace::apply_selection(const std::set<EditorObjectId>& hits, SelectionOperation operation) noexcept {
    if (operation == SelectionOperation::Replace) selection_.clear();
    if (operation == SelectionOperation::Intersect) {
        std::erase_if(selection_, [&](EditorObjectId id) { return !hits.contains(id); });
    } else {
        for (EditorObjectId id : hits) {
            if (!document_.find_object(id)) continue;
            if (operation == SelectionOperation::Subtract) selection_.erase(id);
            else if (operation == SelectionOperation::Toggle) {
                if (selection_.contains(id)) selection_.erase(id);
                else selection_.insert(id);
            } else selection_.insert(id);
        }
    }
    if (!primarySelection_ || !selection_.contains(*primarySelection_)) {
        primarySelection_ = selection_.empty() ? std::nullopt
            : std::optional<EditorObjectId>(*selection_.rbegin());
    }
}

void EditorWorkspace::clear_selection() noexcept {
    selection_.clear();
    primarySelection_.reset();
}

void EditorWorkspace::prune_selection() noexcept {
    std::erase_if(selection_, [&](EditorObjectId id) { return document_.find_object(id) == nullptr; });
    if (primarySelection_ && document_.find_object(*primarySelection_)) return;
    primarySelection_ = selection_.empty()
        ? std::nullopt
        : std::optional<EditorObjectId>(*selection_.rbegin());
}

bool EditorWorkspace::restore_pre_simulation(std::string* error) {
    if (!preSimulationSnapshot_) { if (error) *error = "no pre-simulation snapshot"; return false; }
    document_ = std::move(*preSimulationSnapshot_);
    preSimulationSnapshot_.reset();
    selection_ = std::move(preSimulationSelection_);
    primarySelection_ = preSimulationPrimarySelection_;
    preSimulationSelection_.clear();
    preSimulationPrimarySelection_.reset();
    mode_ = EditorMode::Edit;
    commands_.clear();
    prune_selection();
    return true;
}

bool EditorWorkspace::accept_simulation_state(std::string* error) {
    if (!preSimulationSnapshot_) {
        if (error) *error = "no pre-simulation snapshot";
        return false;
    }
    preSimulationSnapshot_.reset();
    preSimulationSelection_.clear();
    preSimulationPrimarySelection_.reset();
    mode_ = EditorMode::Edit;
    commands_.clear();
    document_.mark_dirty();
    prune_selection();
    return true;
}

} // namespace dve::editor
