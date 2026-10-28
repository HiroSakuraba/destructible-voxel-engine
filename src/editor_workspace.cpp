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
            // A relevant disabled command must remain discoverable with its reason.
            // A large penalty let dozens of weak subsequence matches bury exact Copy.
            // Keep enabled commands first at equal relevance, without hiding exact hits.
            if (!action.enabled) score += tokens.empty() ? 500 : 24;
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
        {"file.import","File","Import Model","Ctrl+I"}, {"file.reimport","File","Reimport Model",""}, {"file.export_print_stl","File","Export for Printing (STL)",""},
        {"file.export_exact_step","File","Export Exact CAD (STEP)...",""},
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
        {"transform.place_mesh_vertex","Edit","Snap to Mesh Vertex",""},
        {"transform.place_mesh_edge","Edit","Snap to Mesh Edge Midpoint",""},
        {"transform.place_mesh_face","Edit","Snap to Mesh Face Centre",""},
        {"transform.align_surface","Edit","Align Up to Surface Normal",""},
        {"transform.surface_offset_more","Edit","Increase Surface Offset",""},
        {"transform.surface_offset_less","Edit","Decrease Surface Offset",""},
        {"transform.scale_double","Edit","Double Voxel Object Size",""},
        {"transform.scale_half","Edit","Halve Voxel Object Size",""},
        {"transform.scale_voxel_size_up","Edit","Scale Voxel Size x2",""},
        {"transform.scale_voxel_size_down","Edit","Scale Voxel Size x0.5",""},
        {"edit.duplicate","Edit","Duplicate","Ctrl+D"}, {"edit.rename","Edit","Rename","F2"},
        {"edit.duplicate_special","Edit","Duplicate Special...","Ctrl+Shift+D"},
        {"edit.group","Edit","Group Selected","Ctrl+G"}, {"edit.ungroup","Edit","Ungroup","Ctrl+Shift+G"},
        {"edit.open_group","Edit","Open Group",""},
        {"edit.close_group","Edit","Close Group","Ctrl+Alt+G"},
        {"edit.close_all_groups","Edit","Close All Groups",""},
        {"edit.edit_pivot","Edit","Edit Pivot",""},
        {"edit.pivot_bottom_center","Edit","Pivot to Bottom Centre",""},
        {"edit.pivot_center","Edit","Pivot to Centre",""},
        {"edit.pivot_to_cursor","Edit","Pivot to Point Under Cursor",""},
        {"select.parent","Edit","Select Parent",""},
        {"select.children","Edit","Select Children",""},
        {"select.siblings","Edit","Select Siblings",""},
        {"select.walk_up","Edit","Pick Walk Up (parent / grow)",""},
        {"select.walk_down","Edit","Pick Walk Down (first child / shrink)",""},
        {"select.walk_left","Edit","Pick Walk Left (previous sibling)",""},
        {"select.walk_right","Edit","Pick Walk Right (next sibling)",""},
        {"edit.mirror_copy_x","Edit","Mirror Copy X",""},
        {"edit.mirror_copy_y","Edit","Mirror Copy Y",""},
        {"edit.mirror_copy_z","Edit","Mirror Copy Z",""},
        {"edit.mirror_x","Edit","Mirror X (in place)",""},
        {"edit.mirror_y","Edit","Mirror Y (in place)",""},
        {"edit.mirror_z","Edit","Mirror Z (in place)",""},
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
        {"view.drop_to_surface","View","Drop Selection to Surface",""},
        {"view.follow_selection","View","Follow Selection",""},
        {"hierarchy.reveal_selection","View","Reveal Selection in Hierarchy",""},
        {"hierarchy.expand_all","View","Expand All Hierarchy Rows",""},
        {"hierarchy.collapse_all","View","Collapse All Hierarchy Rows",""},
        {"view.lit","View","Shading: Lit",""}, {"view.unlit","View","Shading: Unlit",""},
        {"view.wireframe","View","Shading: Wireframe",""}, {"view.material_ids","View","Shading: Material IDs",""},
        {"view.normals","View","Shading: Normals",""}, {"view.lighting_only","View","Shading: Lighting Only",""},
        {"view.overdraw","View","Shading: Overdraw",""}, {"view.voxel_debug","View","Shading: Voxel Debug",""},
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
        {"view.measure","View","Measure",""},
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
        {"create.polygon","Create","Polygon Cube","Ctrl+Shift+M"}, {"create.camera","Create","Camera Rig",""},
        {"create.text3d","Create","3D Text","Ctrl+Shift+T"},
        {"create.gabor_empty","Create","Volume: Empty Gabor Volume",""},
        {"create.gabor_import","Create","Volume: Import Gabor Field",""},
        {"create.prefab_from_selection","Create","Prefab from Selection","Ctrl+Shift+P"},
        {"create.scatter","Create","Scatter Objects",""},
        {"scatter.brush","Create","Scatter Brush",""},
        {"scatter.set_sources","Create","Set Scatter Sources",""},
        {"scatter.clear_sources","Create","Clear Scatter Sources",""},
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
        {"select.domain_objects","Edit","Select Objects",""},
        {"select.domain_voxels","Edit","Select Voxels",""},
        {"select.domain_toggle","Edit","Toggle Objects / Voxels",""},
        {"edit.select_invert","Edit","Invert Selection",""},
        {"edit.select_grow","Edit","Grow Selection",""},
        {"edit.select_shrink","Edit","Shrink Selection",""},
        {"edit.select_connected","Edit","Select Connected",""},
        {"select.set_save","Edit","Save Selection as Set",""},
        {"select.set_save_query","Edit","Save Filter as Smart Set",""},
        {"select.set_recall","Edit","Recall Selection Set...",""},
        {"select.set_add","Edit","Add Selection to Set...",""},
        {"select.set_rename","Edit","Rename Selection Set...",""},
        {"select.set_delete","Edit","Delete Selection Set...",""},
        {"select.matching","Edit","Select Matching Filter",""},
        {"select.similar_type","Edit","Select Similar: Same Type",""},
        {"select.similar_material","Edit","Select Similar: Same Material",""},
        {"select.similar_tag","Edit","Select Similar: Same Tag",""},
        {"select.similar_layer","Edit","Select Similar: Same Layer",""},
        {"select.similar_prefab","Edit","Select Similar: Same Prefab",""},
        {"select.mask_locked","Edit","Selectable: Locked Objects",""},
        {"select.mask_helpers","Edit","Selectable: Helpers (cameras, lights, emitters)",""},
        {"select.mask_prefabs","Edit","Selectable: Prefab Instances",""},
        {"select.mask_all","Edit","Make Everything Selectable",""},
        {"select.shape_box","Edit","Selection Shape: Box",""},
        {"select.shape_lasso","Edit","Selection Shape: Lasso",""},
        {"select.shape_paint","Edit","Selection Shape: Paint",""},
        {"select.shape_cycle","Edit","Next Selection Shape",""},
        {"select.paint_radius_increase","Edit","Larger Paint Selection Brush",""},
        {"select.paint_radius_decrease","Edit","Smaller Paint Selection Brush",""},
        {"voxel.resample_to_scale","Voxel","Resample Voxels to Scale",""},
        {"voxel.select_all","Voxel","Select All Voxels",""},
        {"voxel.select_none","Voxel","Deselect Voxels",""},
        {"voxel.select_grow","Voxel","Grow Voxel Selection",""},
        {"voxel.select_shrink","Voxel","Shrink Voxel Selection",""},
        {"voxel.select_invert","Voxel","Invert Voxel Selection",""},
        {"voxel.selection_brush","Voxel","Selection Brush",""},
        {"voxel.brush_shape_cube","Voxel","Brush Shape: Cube",""},
        {"voxel.brush_shape_sphere","Voxel","Brush Shape: Sphere",""},
        {"voxel.brush_shape_toggle","Voxel","Toggle Brush Shape (Cube / Sphere)",""},
        {"polygon.convert_to_voxels","Polygon","Convert Selected Mesh to Voxels",""},
        {"voxel.convert_to_polygon","Voxel","Convert Selected Voxels to Polygon",""},
        {"voxel.authoring_apply","Voxel","Apply Voxel / Conversion Preview",""},
        {"voxel.authoring_cancel","Voxel","Cancel Voxel / Conversion Preview",""},
        {"voxel.select_cells","Voxel","Click Selection: Single Cell",""},
        {"voxel.select_connected","Voxel","Click Selection: Connected Piece",""},
        {"voxel.extract_selection","Voxel","Extract Selected Voxels to New Object",""},
        {"voxel.region_extrude_x_positive","Voxel","Extrude Region +X (1 Cell)",""},
        {"voxel.region_extrude_x_negative","Voxel","Extrude Region -X (1 Cell)",""},
        {"voxel.region_extrude_y_positive","Voxel","Extrude Region +Y (1 Cell)",""},
        {"voxel.region_extrude_y_negative","Voxel","Extrude Region -Y (1 Cell)",""},
        {"voxel.region_extrude_z_positive","Voxel","Extrude Region +Z (1 Cell)",""},
        {"voxel.region_extrude_z_negative","Voxel","Extrude Region -Z (1 Cell)",""},
        {"voxel.region_rotate_x_positive","Voxel","Rotate Region X +90 Degrees",""},
        {"voxel.region_rotate_x_negative","Voxel","Rotate Region X -90 Degrees",""},
        {"voxel.region_rotate_y_positive","Voxel","Rotate Region Y +90 Degrees",""},
        {"voxel.region_rotate_y_negative","Voxel","Rotate Region Y -90 Degrees",""},
        {"voxel.region_rotate_z_positive","Voxel","Rotate Region Z +90 Degrees",""},
        {"voxel.region_rotate_z_negative","Voxel","Rotate Region Z -90 Degrees",""},
        {"voxel.region_scale_2","Voxel","Scale Region x2",""},
        {"voxel.region_scale_3","Voxel","Scale Region x3",""},
        {"voxel.region_scale_4","Voxel","Scale Region x4",""},
        {"voxel.fill","Voxel","Flood Fill",""},
        {"voxel.rebuild_collision","Voxel","Rebuild Collision",""},
        {"voxel.rebuild_brickmaps","Voxel","Rebuild Brickmaps",""},
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
        {"prototype.kit","Tools","Gameplay Prototype Kit",""},
        {"create.subdivision","Create","Subdivision Surface",""},
        {"nurbs.create_patch","NURBS","Create Editable Patch",""},
        {"nurbs.create_circle","NURBS","Create Rational Circle",""},
        {"nurbs.create_arc","NURBS","Create Quarter Arc",""},
        {"nurbs.create_profile","NURBS","Create Revolve Profile",""},
        {"nurbs.create_revolve","NURBS","Create Revolved Surface",""},
        {"nurbs.construct_add","NURBS","Append Selected Curve Snapshot",""},
        {"nurbs.construct_reverse","NURBS","Reverse Last Input Direction",""},
        {"nurbs.construct_seam","NURBS","Shift Last Input Closed Seam One Span",""},
        {"nurbs.construct_clear","NURBS","Clear Construction Inputs",""},
        {"nurbs.construct_settings","NURBS","Construction and Split/Isocurve Settings",""},
        {"nurbs.construct_close","NURBS","Close Construction Settings",""},
        {"nurbs.construct_loft","NURBS","Ruled Loft through Ordered Snapshots",""},
        {"nurbs.construct_sweep","NURBS","Sampled Sweep (Profile then Rail)",""},
        {"nurbs.extrude","NURBS","Extrude Selected Curve (2 m Y)",""},
        {"nurbs.revolve","NURBS","Revolve Selected Curve (Y Axis)",""},
        {"nurbs.edit","NURBS","Edit Control Points and Weights",""},
        {"nurbs.previous","NURBS","Previous Control Point",""},
        {"nurbs.next","NURBS","Next Control Point",""},
        {"nurbs.finish","NURBS","Finish Control Editing",""},
        {"nurbs.select_all","NURBS","Select All Controls",""},
        {"nurbs.select_none","NURBS","Select No Controls",""},
        {"nurbs.select_invert","NURBS","Invert Control Selection",""},
        {"nurbs.select_row","NURBS","Select U Row through Active Control",""},
        {"nurbs.select_column","NURBS","Select V Column through Active Control",""},
        {"nurbs.construct_smooth_loft","NURBS","Smooth Interpolating Loft from Snapshots",""},
        {"nurbs.source_rebuild","NURBS","Rebuild Curve (0.01 Local Sample Error)",""},
        {"nurbs.source_adaptive","NURBS","Bake Adaptive Surface Polygon (0.01 Local)",""},
        {"nurbs.attach_reference_umin","NURBS","Active reference boundary: U minimum",""},
        {"nurbs.attach_reference_umax","NURBS","Active reference boundary: U maximum",""},
        {"nurbs.attach_reference_vmin","NURBS","Active reference boundary: V minimum",""},
        {"nurbs.attach_reference_vmax","NURBS","Active reference boundary: V maximum",""},
        {"nurbs.attach_moving_umin","NURBS","Other moving boundary: U minimum",""},
        {"nurbs.attach_moving_umax","NURBS","Other moving boundary: U maximum",""},
        {"nurbs.attach_moving_vmin","NURBS","Other moving boundary: V minimum",""},
        {"nurbs.attach_moving_vmax","NURBS","Other moving boundary: V maximum",""},
        {"nurbs.attach_reverse","NURBS","Reverse Boundary Correspondence (Twist)",""},
        {"nurbs.seam_link_c0","NURBS","Link Selected Boundaries (C0, Shared Samples)",""},
        {"nurbs.seam_link_c1","NURBS","Link Selected Boundaries (C1, Shared Samples)",""},
        {"nurbs.seam_refresh","NURBS","Rebuild Seam Network",""},
        {"nurbs.seam_unlink","NURBS","Unlink Seam (Keep Matched Geometry)",""},
        {"nurbs.source_attach_c0","NURBS","Match Other Boundary to Active (C0)",""},
        {"nurbs.source_attach_c1","NURBS","Match Other Boundary to Active (C1)",""},
        {"nurbs.source_project_persistent","NURBS","Project Other Curve as Persistent UV Guide",""},
        {"nurbs.source_curve_refresh","NURBS","Refresh Latest Surface Curve from Projection Source",""},
        {"nurbs.source_curve_remove","NURBS","Remove Latest Surface Curve",""},
        {"nurbs.source_trim_curve","NURBS","Trim with Latest Closed Surface Curve (Add Hole/Loop)",""},
        {"nurbs.source_untrim","NURBS","Untrim Surface (Keep UV Curves)",""},
        {"nurbs.source_trim_invert","NURBS","Invert Retained Trim Regions",""},
        {"nurbs.source_trim_region","NURBS","Keep Hovered Trim Region",""},
        {"nurbs.source_project","NURBS","Project Other Curve Samples onto Active Surface",""},
        {"nurbs.source_intersect_planes","NURBS","Intersect Two Affine Planar Patches",""},
        {"nurbs.source_crop","NURBS","Crop Central Surface by Parameter Margin",""},
        {"nurbs.source_fillet_corner","NURBS","Create Corner Fillet Arc by Leg Fraction",""},
        {"nurbs.source_length","NURBS","Measure Curve Length (World Units)",""},
        {"nurbs.source_elevate_u","NURBS","Elevate U Degree (Keep Shape)",""},
        {"nurbs.source_elevate_v","NURBS","Elevate V Degree (Keep Shape)",""},
        {"nurbs.source_boundaries","NURBS","Extract Four Boundary Curves (Keep Surface)",""},
        {"nurbs.construct_join","NURBS","Join Ordered Curve Snapshots (Matching Ends)",""},
        {"nurbs.source_quality","NURBS","Use Fixed Display Density and Crease Normals",""},
        {"nurbs.source_display_coarse","NURBS","Adaptive Surface Display: Coarse (0.05 Local)",""},
        {"nurbs.source_display_standard","NURBS","Adaptive Surface Display: Standard (0.01 Local)",""},
        {"nurbs.source_display_fine","NURBS","Adaptive Surface Display: Fine (0.0025 Local)",""},
        {"nurbs.source_reset_uv","NURBS","Reset Surface Texture Ranges to 0..1",""},
        {"nurbs.source_split_length","NURBS","Split Curve at Length Fraction (Keep Source)",""},
        {"nurbs.source_split_u","NURBS","Split U at Parameter (Keep Source)",""},
        {"nurbs.source_split_v","NURBS","Split V at Parameter (Keep Source)",""},
        {"nurbs.source_iso_u","NURBS","Extract Isocurve at Fixed U",""},
        {"nurbs.source_iso_v","NURBS","Extract Isocurve at Fixed V",""},
        {"nurbs.source_reverse_u","NURBS","Reverse U Direction",""},
        {"nurbs.source_reverse_v","NURBS","Reverse V Direction",""},
        {"nurbs.source_swap_uv","NURBS","Swap Surface U/V",""},
        {"nurbs.source_insert_u","NURBS","Insert U Knot at Parameter",""},
        {"nurbs.source_insert_v","NURBS","Insert V Knot at Parameter",""},
        {"nurbs.insert_u","NURBS","Insert U Knot at Domain Midpoint",""},
        {"nurbs.insert_v","NURBS","Insert V Knot at Domain Midpoint",""},
        {"nurbs.bake_polygon","NURBS","Bake to New Polygon (Keep Source)",""},
        {"polygon.import","Polygon","Import Polygon Model",""}, {"polygon.create_plane","Polygon","Create Mesh Plane",""},
        {"polygon.import_exact","Polygon","Import Exact CAD (STEP, BREP, USD)",""},
        {"polygon.create_exact_tube","Polygon","Create Exact Tube (CAD)...",""},
        {"polygon.lod","Polygon","LOD Settings",""},
        {"polygon.collision","Polygon","Collision Settings",""}, {"polygon.streaming","Polygon","Streaming Settings",""},
        {"material.library","Materials","Material Library",""}, {"material.globals","Materials","Global Parameters",""},
        {"material.layers","Materials","Material Layers",""}, {"material.rendering","Materials","Shading Models",""},
        {"material.weather","Materials","Add Weathering to Selection",""}, {"material.weather_kind","Materials","Next Weathering Kind",""},
        {"material.import_base_color_map","Materials","Import Base Colour Map",""}, {"material.import_orm_map","Materials","Import ORM Map",""},
        {"material.import_height_map","Materials","Import Height Map",""}, {"material.import_emissive_map","Materials","Import Emission Map",""},
        {"material.clear_maps","Materials","Clear Texture Maps",""},
        // Polygon authoring and component modes; availability is enforced by the native controller.
        {"subdiv.create_cube","Subdivision","Create Subdivision Cube",""},
        {"subdiv.create_plane","Subdivision","Create Subdivision Plane",""},
        {"subdiv.from_polygon","Subdivision","Convert Polygon to Subdivision",""},
        {"subdiv.from_voxels","Subdivision","Convert Voxels to Subdivision",""},
        {"subdiv.bake_polygon","Subdivision","Bake Evaluated Surface to Polygon",""},
        {"subdiv.to_voxels","Subdivision","Convert Evaluated Surface to Voxels",""},
        {"subdiv.level_0","Subdivision","Level 0: Control Cage",""},
        {"subdiv.level_1","Subdivision","Level 1",""},
        {"subdiv.level_2","Subdivision","Level 2",""},
        {"subdiv.level_3","Subdivision","Level 3",""},
        {"subdiv.level_4","Subdivision","Level 4",""},
        {"subdiv.level_up","Subdivision","Increase Subdivision Level",""},
        {"subdiv.level_down","Subdivision","Decrease Subdivision Level",""},
        {"subdiv.sharp_toggle","Subdivision","Preserve Sharp Cage Edges (60 deg)",""},
        {"subdiv.cage_toggle","Subdivision","Show Control Cage",""},
        {"subdiv.cage_xray","Subdivision","Cage X-ray Selection",""},
        {"subdiv.cage_only","Subdivision","Preview Selected Cage Only",""},
        {"subdiv.make_quad","Subdivision","Make Selected Control Quad",""},
        {"subdiv.triangulate_faces","Subdivision","Triangulate Selected Control Faces",""},
        {"subdiv.crease_clear","Subdivision","Clear Selected Edge Creases",""},
        {"subdiv.crease_half","Subdivision","Selected Crease Strength: 0.5",""},
        {"subdiv.crease_1","Subdivision","Selected Crease Strength: 1",""},
        {"subdiv.crease_2","Subdivision","Selected Crease Strength: 2",""},
        {"subdiv.crease_3","Subdivision","Selected Crease Strength: 3",""},
        {"subdiv.crease_4","Subdivision","Selected Crease Strength: 4",""},
        {"subdiv.conversion_apply","Subdivision","Apply Conversion Preview",""},
        {"subdiv.conversion_cancel","Subdivision","Cancel Conversion Preview",""},
        {"subdiv.vertex_mode","Subdivision","Vertex Selection",""},
        {"subdiv.edge_mode","Subdivision","Edge Selection",""},
        {"subdiv.face_mode","Subdivision","Face Selection",""},
        {"subdiv.object_mode","Subdivision","Object Selection",""},
        {"subdiv.move","Subdivision","Move Cage / Object",""},
        {"subdiv.rotate","Subdivision","Rotate Cage / Object",""},
        {"subdiv.scale","Subdivision","Scale Cage / Object",""},
        {"subdiv.select_all","Subdivision","Select All Components",""},
        {"subdiv.select_loop","Subdivision","Select Edge Loops",""},
        {"subdiv.select_ring","Subdivision","Select Edge Rings",""},
        {"subdiv.loop_cut","Subdivision","Loop Cut and Slide",""},
        {"subdiv.dissolve_edges","Subdivision","Dissolve Selected Cage Edges",""},
        {"subdiv.clean_unused","Subdivision","Clean Unused Cage Vertices",""},
        {"subdiv.select_grow","Subdivision","Grow Selection",""},
        {"subdiv.select_shrink","Subdivision","Shrink Selection",""},
        {"subdiv.select_connected","Subdivision","Select Connected",""},
        {"subdiv.select_invert","Subdivision","Invert Selection",""},
        {"subdiv.select_none","Subdivision","Clear Selection",""},
        {"subdiv.extrude","Subdivision","Extrude Control Faces",""},
        {"subdiv.inset","Subdivision","Inset Control Faces",""},
        {"subdiv.bevel","Subdivision","Bevel Control Faces",""},
        {"subdiv.bevel_edges","Subdivision","Bevel Cage Edges: Options (Convex)",""},
        {"subdiv.chamfer_edges","Subdivision","Chamfer Cage Edges (Convex)",""},
        {"subdiv.fillet_edges","Subdivision","Rounded Fillet Cage Edges (Convex)",""},
        {"subdiv.weld","Subdivision","Merge Cage Vertices by Distance",""},
        {"subdiv.dissolve_vertices","Subdivision","Dissolve Redundant Cage Vertices",""},
        {"subdiv.split_edges","Subdivision","Split Cage Corners",""},
        {"subdiv.subdivide_cage","Subdivision","Refine Selected Cage Faces",""},
        {"subdiv.bridge","Subdivision","Bridge Cage Boundaries",""},
        {"subdiv.cap","Subdivision","Cap Cage Boundary",""},
        {"subdiv.extract","Subdivision","Extract Control Faces",""},
        {"subdiv.join","Subdivision","Join Control Cages",""},
        {"subdiv.separate","Subdivision","Separate Cage Shells",""},
        {"subdiv.slice","Subdivision","Slice Control Cage",""},
        {"subdiv.assign_material","Subdivision","Assign Material to Control Faces",""},
        {"subdiv.uv_editor","Subdivision","UV Workspace",""},
        {"subdiv.uv_mark_seam","Subdivision","Mark UV Seam",""},
        {"subdiv.uv_clear_seam","Subdivision","Clear UV Seam",""},
        {"subdiv.uv_toggle_pin","Subdivision","Toggle Saved UV Pins",""},
        {"subdiv.uv_reset_flags","Subdivision","Reset Saved UV Seams and Pins",""},
        {"subdiv.diagnostics","Subdivision","Cage Topology Diagnostics",""},
        {"subdiv.boolean_union","Subdivision","Boolean Union",""},
        {"subdiv.boolean_difference","Subdivision","Boolean A minus B",""},
        {"subdiv.boolean_reverse","Subdivision","Boolean B minus A",""},
        {"subdiv.boolean_intersection","Subdivision","Boolean Intersection",""},
        {"subdiv.hull_fit","Subdivision","Fit Collision Hull",""},
        {"subdiv.hull_edit","Subdivision","Edit Collision Hull",""},
        {"subdiv.hull_remove","Subdivision","Remove Collision Hull",""},
        {"subdiv.preview_apply","Subdivision","Apply Modeling Preview",""},
        {"subdiv.preview_cancel","Subdivision","Cancel Modeling Preview",""},
        {"subdiv.delete","Subdivision","Delete Selection",""},
        {"subdiv.duplicate","Subdivision","Duplicate Selection",""},
        {"polygon.vertex_mode","Polygon","Vertex Selection Mode",""}, {"polygon.edge_mode","Polygon","Edge Selection Mode",""},
        {"polygon.face_mode","Polygon","Face Selection Mode",""}, {"polygon.object_mode","Polygon","Object Selection Mode",""},
        {"polygon.component_toggle","Polygon","Toggle Object or Component Editing",""},
        {"polygon.bevel","Polygon","Face Bevel (tapered extrusion)",""}, {"polygon.extrude","Polygon","Extrude",""}, {"polygon.inset","Polygon","Inset",""},
        {"polygon.uv_tools","Polygon","UV Tools",""}, {"polygon.recalculate_normals","Polygon","Recalculate Normals",""},
        {"polygon.flip_normals","Polygon","Flip Normals",""},
        {"mesh.boolean_union","Polygon","Mesh Boolean Union",""},
        {"mesh.boolean_difference","Polygon","Mesh Boolean A minus B",""},
        {"mesh.boolean_reverse","Polygon","Mesh Boolean B minus A",""},
        {"mesh.boolean_intersection","Polygon","Mesh Boolean Intersection",""},
        {"mesh.exact_boolean_union","Polygon","Exact Boolean Union (CAD)",""},
        {"mesh.exact_boolean_difference","Polygon","Exact Boolean A minus B (CAD)",""},
        {"mesh.exact_boolean_reverse","Polygon","Exact Boolean B minus A (CAD)",""},
        {"mesh.exact_boolean_intersection","Polygon","Exact Boolean Intersection (CAD)",""},
        {"mesh.exact_collision","Polygon","Build Exact Convex Collision (CAD)",""},
        {"mesh.exact_cope","Polygon","Cope Tube A to B (Exact CAD)...",""},
        {"mesh.exact_cope_solid","Polygon","Cope to Supplied Exact Solid (No Gap)...",""},
        {"mesh.exact_miter","Polygon","Plane Cut / Miter Tube (Exact CAD)...",""},
        {"mesh.exact_resize_tube","Polygon","Resize Uncut Tube (New Exact CAD Object)...",""},
        {"mesh.hull_fit","Polygon","Fit Convex Collision Hull",""},
        {"mesh.hull_edit","Polygon","Edit Collision Hull Points",""},
        {"mesh.hull_remove","Polygon","Remove Authored Collision Hull",""},
        {"mesh.bevel_edges","Polygon","Bevel Edges: Options (Convex)",""},
        {"mesh.chamfer_edges","Polygon","Chamfer Edges (Convex)",""},
        {"mesh.fillet_edges","Polygon","Rounded Fillet Edges (Convex)",""},
        {"mesh.subdivide","Polygon","Subdivide Selected Faces",""},
        {"mesh.displace","Polygon","Displace from Height Texture (Bake)",""},
        {"mirror.live_x","Polygon","Live Mirror X",""}, {"mirror.live_y","Polygon","Live Mirror Y",""},
        {"mirror.live_z","Polygon","Live Mirror Z",""}, {"mirror.apply_live","Polygon","Apply Live Mirror (weld seam)",""},
        {"mirror.mesh_x","Polygon","Mirror Mesh X (weld seam)",""}, {"mirror.mesh_y","Polygon","Mirror Mesh Y (weld seam)",""},
        {"mirror.mesh_z","Polygon","Mirror Mesh Z (weld seam)",""},
        {"mirror.faces_x","Polygon","Mirror Selected Faces X",""}, {"mirror.faces_y","Polygon","Mirror Selected Faces Y",""},
        {"mirror.faces_z","Polygon","Mirror Selected Faces Z",""},
        {"mesh.split_edges","Polygon","Split Selected Edges",""},
        {"mesh.flip_diagonal","Polygon","Flip Triangle Diagonal",""},
        {"mesh.bridge","Polygon","Bridge Boundary Loops",""},
        {"mesh.cap","Polygon","Cap Boundary Loop",""},
        {"mesh.hard_normals","Polygon","Hard Face Normals",""},
        {"mesh.smooth_normals","Polygon","Smooth Selected Normals",""},
        {"mesh.diagnostics","Polygon","Topology Diagnostics Overlay",""},
        {"mesh.uv_editor","Polygon","UV Workspace",""},
        {"mesh.uv_mark_seam","Polygon","Mark UV Seam",""},
        {"mesh.uv_clear_seam","Polygon","Clear UV Seam",""},
        {"mesh.uv_toggle_pin","Polygon","Toggle Saved UV Pins",""},
        {"mesh.uv_reset_flags","Polygon","Reset Saved UV Seams and Pins",""},
        {"polygon.assign_material","Polygon","Assign Active Material to Faces",""},
        {"polygon.weld","Polygon","Merge Vertices by Distance",""},
        {"polygon.dissolve_vertices","Polygon","Dissolve Redundant Vertices",""},
        {"polygon.cad_detail","Polygon","CAD Detail...",""},
        {"polygon.select_shell","Polygon","Select Connected Components",""},
        {"polygon.select_loop","Polygon","Select Cage Edge Loops",""},
        {"polygon.select_ring","Polygon","Select Cage Edge Rings",""},
        {"polygon.loop_cut","Polygon","Cage Loop Cut and Slide",""},
        {"polygon.dissolve_edges","Polygon","Dissolve Selected Cage Edges",""},
        {"polygon.clean_unused","Polygon","Clean Unused Mesh Vertices",""},
        {"polygon.select_grow","Polygon","Grow Component Selection",""},
        {"polygon.select_shrink","Polygon","Shrink Component Selection",""},
        {"polygon.select_invert","Polygon","Invert Component Selection",""},
        {"polygon.select_none","Polygon","Clear Component Selection",""},
        {"polygon.extract","Polygon","Extract Selected Faces",""},
        {"polygon.slice","Polygon","Slice Polygon Mesh",""},
        {"polygon.join","Polygon","Join Polygon Meshes",""},
        {"polygon.separate","Polygon","Separate Connected Shells",""},
        {"polygon.preview_apply","Polygon","Apply Tool Preview",""},
        {"polygon.preview_cancel","Polygon","Cancel Tool Preview",""},
        {"material.assign","Materials","Assign or Open Material",""}, {"material.apply","Materials","Compile and Apply Material",""},
        {"material.link","Materials","Link Materials or Settings",""},
        {"material.create_instance","Materials","Create Material Instance",""},
        {"material.create_master","Materials","Create Master Material",""},
        {"material.preview","Materials","Preview Material",""}, {"material.pin_preview","Materials","Pin Material Preview",""},
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
    for(auto id:{"nurbs.attach_reference_umin","nurbs.attach_reference_umax","nurbs.attach_reference_vmin","nurbs.attach_reference_vmax"})configure(id,"Attachment Reference Boundary",55,true,"nurbs.attach.reference");
    for(auto id:{"nurbs.attach_moving_umin","nurbs.attach_moving_umax","nurbs.attach_moving_vmin","nurbs.attach_moving_vmax"})configure(id,"Attachment Moving Boundary",55,true,"nurbs.attach.moving");
    configure("nurbs.attach_reverse","Attachment Correspondence",56,true);
    for(auto id:{"nurbs.source_quality","nurbs.source_display_coarse","nurbs.source_display_standard","nurbs.source_display_fine"})configure(id,"Surface Display",60,true,"nurbs.display");
    for (std::string_view id : {"view.lit", "view.unlit", "view.wireframe", "view.material_ids", "view.normals",
                                "view.lighting_only", "view.overdraw", "view.voxel_debug"})
        configure(id, "Shading", 40, true, "viewport.shading");
    configure("view.follow_selection", "Camera", 20, true);
    for (std::string_view id : {"polygon.vertex_mode", "polygon.edge_mode", "polygon.face_mode", "polygon.object_mode",
                                "polygon.component_toggle", "polygon.bevel", "polygon.extrude", "polygon.inset",
                                "polygon.uv_tools", "polygon.recalculate_normals", "polygon.flip_normals"})
        configure(id, "Mesh Editing", 80);
    for (auto id:{"polygon.vertex_mode","polygon.edge_mode","polygon.face_mode","polygon.object_mode"})
        configure(id,"Mesh Editing",80,true,"polygon.selection");
    for(auto id:{"mesh.boolean_union","mesh.boolean_difference","mesh.boolean_reverse","mesh.boolean_intersection"})configure(id,"Mesh Booleans",70);
    for(auto id:{"mesh.exact_boolean_union","mesh.exact_boolean_difference","mesh.exact_boolean_reverse","mesh.exact_boolean_intersection","mesh.exact_collision","mesh.exact_cope","mesh.exact_cope_solid","mesh.exact_miter","mesh.exact_resize_tube"})configure(id,"Exact CAD Booleans",71);
    for(auto id:{"mesh.hull_fit","mesh.hull_edit","mesh.hull_remove"})configure(id,"Collision Hull",75);
    for(auto id:{"mesh.bevel_edges","mesh.subdivide","mesh.displace","mesh.split_edges","mesh.flip_diagonal","mesh.bridge","mesh.cap","mesh.hard_normals","mesh.smooth_normals","mesh.diagnostics","mesh.uv_editor"})configure(id,"Mesh Editing",80);
    for(auto id:{"mesh.uv_editor","mesh.uv_mark_seam","mesh.uv_clear_seam","mesh.uv_toggle_pin","mesh.uv_reset_flags"})configure(id,"UV Editing",82);
    for (auto id:{"polygon.assign_material","polygon.weld","polygon.select_shell","polygon.select_loop","polygon.select_ring","polygon.select_invert","polygon.select_none","polygon.extract",
                 "polygon.join","polygon.separate","polygon.slice","polygon.preview_apply","polygon.preview_cancel"}) configure(id,"Mesh Editing",80);
    for (std::string_view id : {"material.assign", "material.apply", "material.link", "material.create_instance",
                                "material.create_master", "material.preview", "material.pin_preview"})
        configure(id, "Material Editing (not in this editor)", 80);
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
    configure("view.measure", "Overlays", 51, true);
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
    for (std::string_view id : {"mirror.live_x", "mirror.live_y", "mirror.live_z"}) configure(id, "Symmetry", 78, true, "mirror.live");
    for (std::string_view id : {"mirror.apply_live", "mirror.mesh_x", "mirror.mesh_y", "mirror.mesh_z", "mirror.faces_x", "mirror.faces_y", "mirror.faces_z"})
        configure(id, "Symmetry", 79);
    for (std::string_view id : {"edit.mirror_copy_x", "edit.mirror_copy_y", "edit.mirror_copy_z", "edit.mirror_x", "edit.mirror_y", "edit.mirror_z"})
        configure(id, "Mirror", 24);
    configure("select.domain_objects", "Selection Domain", 30, true, "select.domain");
    configure("select.domain_voxels", "Selection Domain", 31, true, "select.domain");
    configure("select.domain_toggle", "Selection Domain", 32);
    for (std::string_view id : {"select.set_save", "select.set_save_query", "select.set_recall", "select.set_add",
                                "select.set_rename", "select.set_delete"})
        configure(id, "Selection Sets", 39);
    for (std::string_view id : {"select.matching", "select.similar_type", "select.similar_material", "select.similar_tag",
                                "select.similar_layer", "select.similar_prefab"})
        configure(id, "Select By", 38);
    for (std::string_view id : {"edit.select_invert", "edit.select_grow", "edit.select_shrink", "edit.select_connected"})
        configure(id, "Select", 33);
    for (std::string_view id : {"select.mask_locked", "select.mask_helpers", "select.mask_prefabs"}) configure(id, "Selection Mask", 34, true);
    configure("select.mask_all", "Selection Mask", 35);
    for (std::string_view id : {"select.shape_box", "select.shape_lasso", "select.shape_paint"})
        configure(id, "Selection Shape", 36, true, "select.shape");
    for (std::string_view id : {"select.shape_cycle", "select.paint_radius_increase", "select.paint_radius_decrease"})
        configure(id, "Selection Shape", 37);
    for (std::string_view id : {"edit.open_group", "edit.close_group", "edit.close_all_groups"})
        configure(id, "Groups", 26);
    configure("edit.edit_pivot", "Groups", 27, true);
    for (std::string_view id : {"edit.pivot_bottom_center", "edit.pivot_center", "edit.pivot_to_cursor"}) configure(id, "Groups", 27);
    for (std::string_view id : {"voxel.select_all", "voxel.select_none", "voxel.select_grow", "voxel.select_shrink",
                                "voxel.select_invert", "voxel.extract_selection"})
        configure(id, "Voxel Selection", 50);
    configure("subdiv.create_cube","Subdivision Create",0,false);
    configure("subdiv.create_plane","Subdivision Create",1,false);
    configure("subdiv.from_polygon","Subdivision Conversion",2,false);
    configure("subdiv.from_voxels","Subdivision Conversion",3,false);
    configure("subdiv.bake_polygon","Subdivision Conversion",4,false);
    configure("subdiv.to_voxels","Subdivision Conversion",5,false);
    configure("subdiv.level_0","Subdivision Surface",6,true, "subdiv.level");
    configure("subdiv.level_1","Subdivision Surface",7,true, "subdiv.level");
    configure("subdiv.level_2","Subdivision Surface",8,true, "subdiv.level");
    configure("subdiv.level_3","Subdivision Surface",9,true, "subdiv.level");
    configure("subdiv.level_4","Subdivision Surface",10,true, "subdiv.level");
    configure("subdiv.level_up","Subdivision Surface",11,false);
    configure("subdiv.level_down","Subdivision Surface",12,false);
    configure("subdiv.sharp_toggle","Subdivision Surface",13,true);
    configure("subdiv.cage_toggle","Subdivision Surface",14,true);
    configure("subdiv.cage_xray","Subdivision Surface",15,true);
    configure("subdiv.cage_only","Subdivision Surface",15,true);
    configure("subdiv.make_quad","Subdivision Topology",16,false);
    configure("subdiv.triangulate_faces","Subdivision Topology",16,false);
    configure("subdiv.crease_clear","Subdivision Creases",15,false);
    configure("subdiv.crease_half","Subdivision Creases",15,false);
    configure("subdiv.crease_1","Subdivision Creases",15,false);
    configure("subdiv.crease_2","Subdivision Creases",15,false);
    configure("subdiv.crease_3","Subdivision Creases",15,false);
    configure("subdiv.crease_4","Subdivision Creases",15,false);
    configure("subdiv.conversion_apply","Subdivision Conversion",15,false);
    configure("subdiv.conversion_cancel","Subdivision Conversion",16,false);
    configure("subdiv.vertex_mode","Subdivision Selection",30,true, "subdiv.mode");
    configure("subdiv.edge_mode","Subdivision Selection",31,true, "subdiv.mode");
    configure("subdiv.face_mode","Subdivision Selection",32,true, "subdiv.mode");
    configure("subdiv.object_mode","Subdivision Selection",33,true, "subdiv.mode");
    configure("subdiv.move","Subdivision Transform",34,false);
    configure("subdiv.rotate","Subdivision Transform",35,false);
    configure("subdiv.scale","Subdivision Transform",36,false);
    configure("subdiv.select_all","Subdivision Selection",37,false);
    configure("subdiv.select_grow","Subdivision Selection",38,false);
    configure("subdiv.select_loop","Subdivision Selection",38,false);
    configure("subdiv.select_ring","Subdivision Selection",38,false);
    configure("subdiv.loop_cut","Subdivision Modeling",43,false);
    configure("subdiv.dissolve_vertices","Subdivision Modeling",44,false);
    configure("polygon.dissolve_vertices","Mesh Editing",80,false);
    configure("subdiv.dissolve_edges","Subdivision Modeling",44,false);
    configure("subdiv.clean_unused","Subdivision Modeling",45,false);
    configure("polygon.dissolve_edges","Mesh Editing",80,false);
    configure("polygon.clean_unused","Mesh Editing",80,false);
    configure("polygon.loop_cut","Mesh Editing",80,false);
    configure("subdiv.select_shrink","Subdivision Selection",39,false);
    configure("subdiv.select_connected","Subdivision Selection",40,false);
    configure("subdiv.select_invert","Subdivision Selection",41,false);
    configure("subdiv.select_none","Subdivision Selection",42,false);
    configure("subdiv.extrude","Subdivision Modeling",43,false);
    configure("subdiv.inset","Subdivision Modeling",44,false);
    configure("subdiv.bevel","Subdivision Modeling",45,false);
    configure("subdiv.bevel_edges","Subdivision Modeling",46,false);
    configure("subdiv.chamfer_edges","Subdivision Modeling",46,false);
    configure("subdiv.fillet_edges","Subdivision Modeling",46,false);
    configure("mesh.chamfer_edges","Mesh Editing",80);
    configure("mesh.fillet_edges","Mesh Editing",80);
    configure("subdiv.weld","Subdivision Modeling",47,false);
    configure("subdiv.split_edges","Subdivision Topology",48,false);
    configure("subdiv.subdivide_cage","Subdivision Topology",49,false);
    configure("subdiv.bridge","Subdivision Topology",50,false);
    configure("subdiv.cap","Subdivision Topology",51,false);
    configure("subdiv.extract","Subdivision Modeling",52,false);
    configure("subdiv.join","Subdivision Modeling",53,false);
    configure("subdiv.separate","Subdivision Modeling",54,false);
    configure("subdiv.slice","Subdivision Modeling",55,false);
    configure("subdiv.assign_material","Subdivision Materials and UV",56,false);
    configure("subdiv.uv_editor","Subdivision Materials and UV",57,false);
    configure("subdiv.uv_mark_seam","Subdivision Materials and UV",57,false);
    configure("subdiv.uv_clear_seam","Subdivision Materials and UV",57,false);
    configure("subdiv.uv_toggle_pin","Subdivision Materials and UV",57,false);
    configure("subdiv.uv_reset_flags","Subdivision Materials and UV",57,false);
    configure("subdiv.diagnostics","Subdivision Topology",58,false);
    configure("subdiv.boolean_union","Subdivision Booleans",59,false);
    configure("subdiv.boolean_difference","Subdivision Booleans",60,false);
    configure("subdiv.boolean_reverse","Subdivision Booleans",61,false);
    configure("subdiv.boolean_intersection","Subdivision Booleans",62,false);
    configure("subdiv.hull_fit","Subdivision Collision",63,false);
    configure("subdiv.hull_edit","Subdivision Collision",64,false);
    configure("subdiv.hull_remove","Subdivision Collision",65,false);
    configure("subdiv.preview_apply","Subdivision Preview",66,false);
    configure("subdiv.preview_cancel","Subdivision Preview",67,false);
    configure("subdiv.delete","Subdivision Modeling",68,false);
    configure("subdiv.duplicate","Subdivision Modeling",69,false);
    configure("voxel.selection_brush", "Voxel Selection", 51, true);
    configure("voxel.brush_shape_cube", "Brush", 45, true, "voxel.brush_shape");
    configure("voxel.brush_shape_sphere", "Brush", 46, true, "voxel.brush_shape");
    configure("voxel.brush_shape_toggle", "Brush", 47);
    configure("voxel.select_cells", "Voxel Selection", 48, true, "voxel.click_selection");
    configure("voxel.select_connected", "Voxel Selection", 49, true, "voxel.click_selection");
    configure("polygon.convert_to_voxels", "Geometry Conversion", 60);
    configure("voxel.convert_to_polygon", "Geometry Conversion", 60);
    configure("voxel.authoring_apply", "Voxel Preview", 70);
    configure("voxel.authoring_cancel", "Voxel Preview", 71);
    for (std::string_view id : {
            "voxel.region_extrude_x_positive",
            "voxel.region_extrude_x_negative",
            "voxel.region_extrude_y_positive",
            "voxel.region_extrude_y_negative",
            "voxel.region_extrude_z_positive",
            "voxel.region_extrude_z_negative",
            "voxel.region_rotate_x_positive",
            "voxel.region_rotate_x_negative",
            "voxel.region_rotate_y_positive",
            "voxel.region_rotate_y_negative",
            "voxel.region_rotate_z_positive",
            "voxel.region_rotate_z_negative",
            "voxel.region_scale_2",
            "voxel.region_scale_3",
            "voxel.region_scale_4"})
        configure(id, "Voxel Region Editing", 52);
    configure("voxel.fill", "Voxel Edit", 50);
    configure("voxel.rebuild_collision", "Collision", 80);
    configure("voxel.rebuild_brickmaps", "Collision", 81);
    configure("view.drop_to_surface", "Camera", 21);
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
    // Material instance commands remain unavailable and appear only in Advanced Menus.
    for (std::string_view id : {"material.assign", "material.apply", "material.link", "material.create_instance",
                                "material.create_master", "material.preview", "material.pin_preview"})
        advanced(id);
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
    describe("voxel.resample_to_scale", "Rebuild the selected voxel objects' cells at their current Scale, then set Scale back to 1. Scaling itself keeps the voxels; this is the step that changes them.", {"resample", "bake", "scale", "voxels", "destructive"});
    describe("transform.place_mesh_vertex", "Move the selection so its pivot lands on the mesh vertex under the cursor; click to place. Or hold V while dragging the Move gizmo.", {"snap", "vertex", "point", "align"});
    describe("transform.place_mesh_edge", "Move the selection so its pivot lands on the middle of the mesh edge under the cursor; click to place.", {"snap", "edge", "midpoint", "align"});
    describe("transform.place_mesh_face", "Move the selection so its pivot lands on the centre of the mesh face under the cursor; click to place.", {"snap", "face", "centre", "center", "align"});
    describe("edit.duplicate_special", "Make many copies, each offset, turned and scaled from the last, around its own pivot or a point (fans, rings, spirals). Preview, then Apply as one undo step.",
             {"array", "copies", "repeat", "radial", "instance", "duplicate"});
    describe("edit.open_group", "Let viewport clicks reach the pieces inside the selected group. Double-clicking a piece does the same.", {"group", "enter", "isolate"});
    describe("edit.close_group", "Close the open group around the selection and select the whole group again.", {"group", "exit"});
    describe("edit.close_all_groups", "Close every open group so clicks select whole groups.", {"group", "exit"});
    describe("edit.edit_pivot", "Move or rotate the selected group's pivot without moving its pieces.", {"group", "pivot", "origin"});
    describe("edit.pivot_bottom_center", "Put the pivot of the selected group, voxel objects or meshes at the bottom centre of their geometry; nothing moves in the world.", {"group", "pivot", "origin"});
    describe("edit.pivot_center", "Put the pivot of the selected group, voxel objects or meshes at the centre of their geometry; nothing moves in the world.", {"group", "pivot", "origin"});
    describe("edit.pivot_to_cursor", "Put the selected objects' pivot on the vertex, edge, face or surface under the cursor (their own included); nothing moves in the world.", {"pivot", "origin", "snap", "cursor"});
    describe("subdiv.vertex_mode","Vertex Selection through the shared control-cage selection workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.edge_mode","Edge Selection through the shared control-cage selection workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.face_mode","Face Selection through the shared control-cage selection workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.object_mode","Object Selection through the shared control-cage selection workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.move","Move Cage / Object through the shared control-cage transform workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.rotate","Rotate Cage / Object through the shared control-cage transform workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.scale","Scale Cage / Object through the shared control-cage transform workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.select_all","Select All Components through the shared control-cage selection workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.select_grow","Grow Selection through the shared control-cage selection workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("select.walk_up", "Select the parent (mesh components and voxels: grow by one ring). The camera stays put.", {"pickwalk", "parent", "hierarchy", "grow"});
    describe("select.walk_down", "Select the first child (mesh components and voxels: shrink by one ring).", {"pickwalk", "child", "hierarchy", "shrink"});
    describe("select.walk_left", "Select the previous sibling (mesh components: the neighbour to the left).", {"pickwalk", "sibling", "previous"});
    describe("select.walk_right", "Select the next sibling (mesh components: the neighbour to the right).", {"pickwalk", "sibling", "next"});
    for (const char* axis : {"x", "y", "z"}) {
        describe(std::string("mirror.live_") + axis, "Show the mesh mirrored across its own axis while you model one half; Apply Live Mirror welds it.",
                 {"symmetry", "mirror", "half", "modifier"});
        describe(std::string("mirror.mesh_") + axis, "Mirror the whole mesh across its own axis (through its origin) and weld the centre seam.",
                 {"symmetry", "mirror", "half", "weld"});
        describe(std::string("mirror.faces_") + axis, "Add a mirrored duplicate of the selected faces across the mesh's own axis, welded where they touch the plane.",
                 {"symmetry", "mirror", "faces"});
    }
    for (const char* axis : {"x", "y", "z"}) {
        describe(std::string("edit.mirror_copy_") + axis, "Make a mirrored copy of the selection across the world plane through the pivot (voxels and meshes really flip).",
                 {"mirror", "symmetry", "flip", "reflect", "copy"});
        describe(std::string("edit.mirror_") + axis, "Mirror the selection in place across the world plane through the pivot (voxels and meshes really flip).",
                 {"mirror", "symmetry", "flip", "reflect"});
    }
    describe("file.export_exact_step", "Write the selected CAD objects into one STEP file in exports/, each a named solid placed where it is in the scene, from its exact source. Choose millimetres or metres, Z or Y up. The file is read back and checked; nothing is written if that fails. Stretched objects are exported converted and named.", {"step", "stp", "export", "cad", "brep", "exact", "solid", "mechanical"});
    describe("view.measure", "Click two points for the distance between them; snaps to vertices, edge midpoints and, on CAD objects, exact corners and circle centres. CAD points read in millimetres and clicking a CAD face names it (Hole \xC3\x98 12.000 mm). Esc ends.", {"measure", "distance", "ruler", "dimension", "diameter", "radius", "cad"});
    describe("polygon.create_exact_tube", "Create a hollow round, square or rectangular tube as an exact B-rep (outer solid minus the bore, certified by brepkernel), e.g. round 50.8 3 600 y or rect 80x40 3 600 r 6 y. Optional r specifies outer corner radius in mm; undo removes it.", {"tube", "pipe", "hollow", "round", "square", "rectangular", "cad", "exact", "wall"});
    describe("mesh.exact_cope", "Cope tube A (active) to tube B: cut away B's solid outer body from A with the exact Boolean, so A's end fits B's outside. Type an optional fit-up gap in mm. Makes a new object; A and B are unchanged. Equal diameters at 90 degrees are cut with an automatic 0.01 mm clearance, and the result says so.", {"cope", "fish mouth", "fishmouth", "notch", "saddle", "tube", "joint", "cad", "exact"});
    describe("mesh.exact_cope_solid", "Cut A by B's actual exact solid, at zero gap. For tapered or variable-width cutters, supply the solid outer envelope; a hollow tube's interior is not filled automatically. Makes a new object.", {"cope", "taper", "solid", "cad", "exact"});
    describe("mesh.exact_resize_tube", "Regenerate an uncut tube with new dimensions as a new exact CAD object at the original pose. Existing cuts cannot yet be replayed; the original is unchanged.", {"tube", "resize", "cad", "exact", "stock"});
    describe("mesh.exact_miter", "Cut a tube on a plane through both walls at an angle from a square cut (e.g. 47.5), keeping the low or high end, with the exact Boolean. Makes a new object; the tube is unchanged.", {"miter", "mitre", "plane cut", "angle cut", "bevel", "tube", "cad", "exact"});
    describe("polygon.cad_detail", "Re-mesh CAD objects from their exact source so the triangles stay within a chosen distance of the true surface (1 mm to 0.005 mm, or typed). The exact source does not change.", {"cad", "accuracy", "tessellation", "deflection", "detail", "exact"});
    describe("select.set_save", "Save the selected objects, or the selected vertices, edges or faces of a mesh, as a named set saved with the scene.", {"selection set", "named", "save", "store"});
    describe("select.set_save_query", "Save the hierarchy filter (a name or a query such as type:mesh tag:rock) as a smart set that is re-evaluated each time.", {"selection set", "smart", "query", "saved search"});
    describe("select.set_recall", "Pick a selection set to select again; shows how many members are missing or retired.", {"selection set", "recall", "restore"});
    describe("select.set_add", "Pick a selection set and add the current selection to it.", {"selection set", "add", "update"});
    describe("select.set_rename", "Pick a selection set and give it a new name.", {"selection set", "rename"});
    describe("select.set_delete", "Pick a selection set to delete (Undo restores it).", {"selection set", "delete", "remove"});
    describe("select.matching", "Select every object the hierarchy filter matches: a name, or terms like type:mesh tag:rock layer:2 material:stone locked:no (-tag:x excludes).", {"query", "filter", "search", "property"});
    describe("select.similar_type", "Add every visible selectable object of the same kind (voxels, mesh, subdivision, text, helper, group).", {"similar", "same", "kind", "type"});
    describe("select.similar_material", "Add every visible selectable object that uses one of the selection's materials.", {"similar", "same", "material"});
    describe("select.similar_tag", "Add every visible selectable object sharing a tag with the selection.", {"similar", "same", "tag"});
    describe("select.similar_layer", "Add every visible selectable object on the selection's layers.", {"similar", "same", "layer"});
    describe("select.similar_prefab", "Add every visible selectable instance of the selection's prefabs.", {"similar", "same", "prefab", "instance"});
    describe("edit.select_invert", "Swap selected and unselected in the active domain: objects, voxel cells or mesh components.", {"inverse", "invert", "deselect"});
    describe("edit.select_grow", "Grow the selection in the active domain: neighbouring cells or components; for objects, add their children.", {"expand", "grow", "more"});
    describe("edit.select_shrink", "Shrink the selection in the active domain: drop the outer ring; for objects, drop children whose parent is selected.", {"contract", "shrink", "less"});
    describe("edit.select_connected", "Select everything connected: the voxel pieces or mesh shells touched, or the whole hierarchy of each object.", {"island", "shell", "piece", "hierarchy"});
    describe("select.mask_locked", "Let viewport clicks and boxes pick up locked objects. Off: they are skipped (the hierarchy still selects them).", {"mask", "filter", "locked"});
    describe("select.mask_helpers", "Let viewport clicks and boxes pick up helpers: objects without geometry such as cameras, lights and emitters.", {"mask", "filter", "helper", "camera", "light"});
    describe("select.mask_prefabs", "Let viewport clicks and boxes pick up prefab instances.", {"mask", "filter", "prefab"});
    describe("select.mask_all", "Turn every selection mask back on.", {"mask", "filter", "reset"});
    describe("select.shape_box", "Select-tool drags draw a box: left to right contains, right to left crosses.", {"marquee", "rectangle", "box"});
    describe("select.shape_lasso", "Select-tool drags draw a freehand loop; everything inside it is selected (visible only unless X-Ray).", {"lasso", "freehand", "loop"});
    describe("select.shape_paint", "Select-tool drags paint the selection with a round brush; Ctrl-drag removes. Voxels use the Selection Brush.", {"paint", "brush", "radius"});
    describe("select.shape_cycle", "Switch Box, Lasso and Paint selection in turn.", {"lasso", "paint", "box", "shape"});
    describe("select.paint_radius_increase", "Make the paint selection brush larger.", {"paint", "brush", "radius", "size"});
    describe("select.paint_radius_decrease", "Make the paint selection brush smaller.", {"paint", "brush", "radius", "size"});
    describe("subdiv.select_loop","Follow selected cage edges through regular quad vertices; stops at boundaries, poles and triangles.",{"subdivision","edge","loop"});
    describe("subdiv.select_ring","Select opposite edges across saved control quads; stops at boundaries and triangles.",{"subdivision","edge","ring"});
    describe("polygon.select_loop","Select loops on an authored subdivision control cage; convert an ordinary polygon mesh to subdivision first.",{"cage","edge","loop"});
    describe("polygon.select_ring","Select rings on an authored subdivision control cage; convert an ordinary polygon mesh to subdivision first.",{"cage","edge","ring"});
    describe("subdiv.loop_cut","Preview a cut across a planar quad strip and slide it by dragging or typing a fraction; preserve crease and UV lineage.",{"subdivision","cut","slide"});
    describe("polygon.loop_cut","Cut and slide an authored subdivision cage strip with Apply and Cancel.",{"cage","cut","slide"});
    describe("subdiv.dissolve_edges","Preview removing redundant edges between disjoint triangle pairs; protects seams, creases, materials and attribute breaks.",{"subdivision","dissolve","cleanup"});
    describe("polygon.dissolve_edges","Dissolve edges on an authored subdivision cage with Apply and Cancel.",{"cage","dissolve"});
    describe("subdiv.clean_unused","Remove unreferenced cage vertices while preserving authored faces, creases and UV flags.",{"subdivision","unused","cleanup"});
    describe("polygon.clean_unused","Remove unused attribute vertices without merging used UV or normal seam copies.",{"mesh","unused","cleanup"});
    describe("subdiv.select_shrink","Shrink Selection through the shared control-cage selection workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.select_connected","Select Connected through the shared control-cage selection workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.select_invert","Invert Selection through the shared control-cage selection workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.select_none","Clear Selection through the shared control-cage selection workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.extrude","Extrude Control Faces through the shared control-cage modeling workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.inset","Inset Control Faces through the shared control-cage modeling workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.bevel","Bevel Control Faces through the shared control-cage modeling workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.bevel_edges","Bevel Cage Edges (Convex) through the shared control-cage modeling workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("polygon.weld","Preview merging selected vertices to existing anchors within a mesh-space distance; retains seam attributes and refuses collapsed or nonmanifold faces.",{"merge","weld","distance","cleanup"});
    describe("polygon.dissolve_vertices","Preview removing straight boundary vertices in disjoint two-triangle fans; protects seams, creases and interpolated attributes.",{"dissolve","vertex","cleanup"});
    describe("subdiv.dissolve_vertices","Dissolve redundant boundary vertices with explicit control-face and crease mappings.",{"dissolve","subdivision","cleanup"});
    describe("subdiv.weld","Preview merging selected cage vertices by distance while preserving control faces, crease strengths and UV flags.",{"subdivision","subdiv","cage"});
    describe("subdiv.split_edges","Split Cage Edges through the shared control-cage topology workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.subdivide_cage","Refine Selected Cage Faces through the shared control-cage topology workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.bridge","Bridge Cage Boundaries through the shared control-cage topology workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.cap","Cap Cage Boundary through the shared control-cage topology workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.extract","Extract Control Faces through the shared control-cage modeling workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.join","Join Control Cages through the shared control-cage modeling workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.separate","Separate Cage Shells through the shared control-cage modeling workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.slice","Slice Control Cage through the shared control-cage modeling workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.assign_material","Assign Material to Control Faces through the shared control-cage materials and uv workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.uv_editor","UV Workspace through the shared control-cage materials and uv workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.uv_mark_seam","Mark UV Seam on the control cage through the shared UV workspace.",{"subdivision","subdiv","cage","uv"});
    describe("subdiv.uv_clear_seam","Clear UV Seam on the control cage through the shared UV workspace.",{"subdivision","subdiv","cage","uv"});
    describe("subdiv.uv_toggle_pin","Toggle Saved UV Pins on the control cage through the shared UV workspace.",{"subdivision","subdiv","cage","uv"});
    describe("subdiv.uv_reset_flags","Reset Saved UV Seams and Pins on the control cage through the shared UV workspace.",{"subdivision","subdiv","cage","uv"});
    describe("subdiv.diagnostics","Cage Topology Diagnostics through the shared control-cage topology workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.boolean_union","Boolean Union through the shared control-cage booleans workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.boolean_difference","Boolean A minus B through the shared control-cage booleans workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.boolean_reverse","Boolean B minus A through the shared control-cage booleans workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.boolean_intersection","Boolean Intersection through the shared control-cage booleans workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.hull_fit","Fit Collision Hull through the shared control-cage collision workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.hull_edit","Edit Collision Hull through the shared control-cage collision workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.hull_remove","Remove Collision Hull through the shared control-cage collision workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.preview_apply","Apply Modeling Preview through the shared control-cage preview workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.preview_cancel","Cancel Modeling Preview through the shared control-cage preview workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.delete","Delete Selection through the shared control-cage modeling workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.duplicate","Duplicate Selection through the shared control-cage modeling workflow; retains subdivision settings when editing the existing cage.",{"subdivision","subdiv","cage"});
    describe("mesh.chamfer_edges","Preview a flat one-segment chamfer with editable width and offset space.",{"mesh","bevel","chamfer"});
    describe("mesh.fillet_edges","Preview a segmented fillet on a convex solid, straight open crease or rolled boundary; unsupported corner joins are refused.",{"mesh","bevel","fillet","round"});
    describe("subdiv.chamfer_edges","Chamfer selected convex cage edges through the shared options preview.",{"subdivision","chamfer"});
    describe("subdiv.fillet_edges","Round selected convex cage edges through the shared options preview.",{"subdivision","fillet","round"});
    describe("create.subdivision","Create an owned Catmull-Clark subdivision surface with an editable cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.create_cube","Create a smooth subdivision cube, retaining its small control cage.",{"subdivision","subdiv","cage"});
    describe("subdiv.create_plane","Create a subdivision plane with interpolated boundary corners.",{"subdivision","subdiv","cage"});
    describe("subdiv.from_polygon","Create a new subdivision surface from the selected polygon cage; retain the source.",{"subdivision","subdiv","cage"});
    describe("subdiv.from_voxels","Preview a voxel block surface as a subdivision cage; choose level 0..4, Apply keeps the voxels.",{"subdivision","subdiv","cage"});
    describe("subdiv.bake_polygon","Create an ordinary editable polygon from the evaluated surface; retain the subdivision source.",{"subdivision","subdiv","cage"});
    describe("subdiv.to_voxels","Preview voxelizing the evaluated smooth surface, with cell size and fill controls.",{"subdivision","subdiv","cage"});
    describe("subdiv.sharp_toggle","Toggle preservation of cage edges whose dihedral angle exceeds 60 degrees.",{"subdivision","subdiv","cage"});
    describe("subdiv.cage_toggle","Show or hide the control-cage overlay without changing geometry.",{"subdivision","subdiv","cage"});
    describe("subdiv.cage_xray","Pick and box-select cage components through the surface, including hidden-side edges.",{"subdivision","cage","crease"});
    describe("subdiv.cage_only","Show the selected object as its flat cage instead of the smooth surface; the saved level is unchanged.",{"subdivision","cage","crease"});
    describe("subdiv.make_quad","Make Selected Control Quad without moving vertices or changing UVs; supports Undo.",{"subdivision","topology","quad"});
    describe("subdiv.triangulate_faces","Triangulate Selected Control Faces without moving vertices or changing UVs; supports Undo.",{"subdivision","topology","quad"});
    describe("subdiv.crease_clear","Remove authored creases from the selected cage edges.",{"subdivision","cage","crease"});
    describe("subdiv.crease_half","Set a soft semi-sharp crease (0.5) on the selected cage edges.",{"subdivision","cage","crease"});
    describe("subdiv.crease_1","Set a crease that stays sharp for one refinement level on the selected cage edges.",{"subdivision","cage","crease"});
    describe("subdiv.crease_2","Set a crease that stays sharp for two refinement levels on the selected cage edges.",{"subdivision","cage","crease"});
    describe("subdiv.crease_3","Set a crease that stays sharp for three refinement levels on the selected cage edges.",{"subdivision","cage","crease"});
    describe("subdiv.crease_4","Set a fully sharp crease (4) on the selected cage edges.",{"subdivision","cage","crease"});
    describe("subdiv.level_up","Increase finite Catmull-Clark refinement while retaining the cage, UVs and materials.",{"subdivision","subdiv","cage"});
    describe("subdiv.level_down","Decrease finite Catmull-Clark refinement while retaining the cage, UVs and materials.",{"subdivision","subdiv","cage"});
    describe("subdiv.conversion_apply","Apply a reviewed conversion result in one geometry/selection Undo step.",{"subdivision","subdiv","cage"});
    describe("subdiv.conversion_cancel","Discard the conversion preview without changing the source.",{"subdivision","subdiv","cage"});
    describe("subdiv.level_0","Set finite subdivision refinement to level 0; zero displays the cage. Rejects excessive or invalid generated geometry.",{"subdivision","level","smooth"});
    describe("subdiv.level_1","Set finite subdivision refinement to level 1; zero displays the cage. Rejects excessive or invalid generated geometry.",{"subdivision","level","smooth"});
    describe("subdiv.level_2","Set finite subdivision refinement to level 2; zero displays the cage. Rejects excessive or invalid generated geometry.",{"subdivision","level","smooth"});
    describe("subdiv.level_3","Set finite subdivision refinement to level 3; zero displays the cage. Rejects excessive or invalid generated geometry.",{"subdivision","level","smooth"});
    describe("subdiv.level_4","Set finite subdivision refinement to level 4; zero displays the cage. Rejects excessive or invalid generated geometry.",{"subdivision","level","smooth"});
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
    describe("scatter.brush",
             "Paint copies of the selected objects (or set scatter sources) onto any voxel surface; Shift-drag erases, Ctrl+wheel sets the radius.",
             {"scatter", "brush", "paint", "foliage", "trees", "rocks", "populate", "erase"});
    describe("scatter.set_sources",
             "Remember the selected objects (and the selected prefab) as what Scatter copies; then select any number of surfaces to scatter onto.",
             {"scatter", "sources", "several surfaces", "multiple", "pick"});
    describe("scatter.clear_sources", "Forget the set scatter sources; Scatter uses the selection again.", {"scatter", "sources", "reset"});
    describe("scatter.cancel", "Close the scatter preview without changing the scene.", {"scatter"});
    describe("voxel.slice", "Preview a cell-centre plane cut; keep front, back, or both as separate objects.", {"slice", "cut", "plane"});
    describe("select.domain_objects", "Select and Move work on whole objects.", {"object mode", "selection mode", "domain"});
    describe("select.domain_voxels",
             "Select cells of one voxel object: left-to-right box contains, right-to-left crosses; Shift adds, Ctrl removes, Ctrl+Shift intersects. Selection Brush is a separate mode.",
             {"voxel mode", "voxel select", "region", "selection mode", "domain", "cells"});
    describe("voxel.extract_selection", "Move the selected voxels into a new object in place (Separate Selected).",
             {"extract", "separate", "split", "detach", "new object", "voxels"});
    describe("polygon.convert_to_voxels", "Preview voxelizing the selected owned polygon mesh; choose cell size and fill mode, Apply creates a new object and keeps the source.",
             {"convert", "voxelize", "polygon", "mesh", "voxels"});
    describe("voxel.convert_to_polygon", "Preview an editable block-surface mesh with material groups and UV0/UV1; Apply creates a new object and keeps the source.",
             {"convert", "surface", "mesh", "polygon", "voxels"});
    describe("voxel.authoring_apply", "Apply the reviewed voxel region or conversion result in one Undo step.", {"apply", "preview", "conversion"});
    describe("voxel.authoring_cancel", "Close the voxel region or conversion preview without changing scene geometry.", {"cancel", "preview", "conversion"});
    describe("voxel.select_cells", "Viewport clicks select individual voxel cells; box gestures stay the same.", {"selection", "cell", "precision", "voxel"});
    describe("voxel.select_connected", "Viewport clicks select the face-connected voxel piece, including buried cells.", {"selection", "piece", "connected", "voxel"});
    describe("voxel.selection_brush", "Toggle voxel Selection Brush: drag adds, Ctrl-drag removes, [ / ] changes radius; turn it off to box-select.",
             {"selection", "brush", "paint", "region", "voxels"});
    describe("voxel.region_extrude_x_positive", "Preview extending selected column frontiers; edit the signed cell distance along object-grid +X; blocked destinations refuse the preview.", {"region", "voxels", "extrude"});
    describe("voxel.region_extrude_x_negative", "Preview extending selected column frontiers; edit the signed cell distance along object-grid -X; blocked destinations refuse the preview.", {"region", "voxels", "extrude"});
    describe("voxel.region_extrude_y_positive", "Preview extending selected column frontiers; edit the signed cell distance along object-grid +Y; blocked destinations refuse the preview.", {"region", "voxels", "extrude"});
    describe("voxel.region_extrude_y_negative", "Preview extending selected column frontiers; edit the signed cell distance along object-grid -Y; blocked destinations refuse the preview.", {"region", "voxels", "extrude"});
    describe("voxel.region_extrude_z_positive", "Preview extending selected column frontiers; edit the signed cell distance along object-grid +Z; blocked destinations refuse the preview.", {"region", "voxels", "extrude"});
    describe("voxel.region_extrude_z_negative", "Preview extending selected column frontiers; edit the signed cell distance along object-grid -Z; blocked destinations refuse the preview.", {"region", "voxels", "extrude"});
    describe("voxel.region_rotate_x_positive", "Preview rotating selected cells +90 degrees around object-grid X, using a snapped cell-centre pivot; blocked destinations refuse the preview.", {"region", "voxels", "rotate"});
    describe("voxel.region_rotate_x_negative", "Preview rotating selected cells -90 degrees around object-grid X, using a snapped cell-centre pivot; blocked destinations refuse the preview.", {"region", "voxels", "rotate"});
    describe("voxel.region_rotate_y_positive", "Preview rotating selected cells +90 degrees around object-grid Y, using a snapped cell-centre pivot; blocked destinations refuse the preview.", {"region", "voxels", "rotate"});
    describe("voxel.region_rotate_y_negative", "Preview rotating selected cells -90 degrees around object-grid Y, using a snapped cell-centre pivot; blocked destinations refuse the preview.", {"region", "voxels", "rotate"});
    describe("voxel.region_rotate_z_positive", "Preview rotating selected cells +90 degrees around object-grid Z, using a snapped cell-centre pivot; blocked destinations refuse the preview.", {"region", "voxels", "rotate"});
    describe("voxel.region_rotate_z_negative", "Preview rotating selected cells -90 degrees around object-grid Z, using a snapped cell-centre pivot; blocked destinations refuse the preview.", {"region", "voxels", "rotate"});
    describe("voxel.region_scale_2", "Preview expanding cells to integer blocks (editable factor 2..8) from the region minimum corner; blocked destinations refuse the preview.", {"region", "voxels", "scale"});
    describe("voxel.region_scale_3", "Preview expanding cells to integer blocks (editable factor 2..8) from the region minimum corner; blocked destinations refuse the preview.", {"region", "voxels", "scale"});
    describe("voxel.region_scale_4", "Preview expanding cells to integer blocks (editable factor 2..8) from the region minimum corner; blocked destinations refuse the preview.", {"region", "voxels", "scale"});
    describe("voxel.select_grow", "Add the voxels touching the selection.", {"grow", "expand", "select"});
    describe("voxel.select_shrink", "Drop the selection's outer layer.", {"shrink", "contract", "select"});
    describe("voxel.select_invert", "Select the object's other voxels instead.", {"invert", "inverse", "select"});
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
        if (action.id.starts_with("transform.") || action.id.starts_with("view.") || action.id == "scatter.brush" ||
            action.id.starts_with("select."))
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
