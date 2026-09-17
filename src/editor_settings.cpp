#include "dve/editor_settings.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>

namespace dve::editor {
namespace {

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return result;
}

bool capabilities_match(const SettingDefinition& definition, std::uint32_t capabilities) noexcept {
    return definition.requiredCapabilities == SettingCapabilityNone ||
           (definition.requiredCapabilities & capabilities) == definition.requiredCapabilities;
}

std::string type_name(SettingType type) {
    switch (type) {
        case SettingType::Boolean: return "bool";
        case SettingType::Integer: return "int";
        case SettingType::Float: return "float";
        case SettingType::Enum: return "enum";
        case SettingType::String: return "string";
    }
    return "unknown";
}

std::optional<SettingType> parse_type(std::string_view value) {
    if (value == "bool") return SettingType::Boolean;
    if (value == "int") return SettingType::Integer;
    if (value == "float") return SettingType::Float;
    if (value == "enum") return SettingType::Enum;
    if (value == "string") return SettingType::String;
    return std::nullopt;
}

template<class T>
bool parse_number(std::string_view text, T& value) {
    const char* begin = text.data();
    const char* end = text.data() + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

SettingDefinition boolean_setting(std::string id, std::string category, std::string section,
                                  std::string label, bool defaultValue, std::string description,
                                  std::uint32_t capability = SettingCapabilityNone,
                                  bool advanced = false,
                                  SettingApplyPolicy policy = SettingApplyPolicy::Live) {
    return {std::move(id), std::move(category), std::move(section), std::move(label),
            std::move(description), SettingType::Boolean, defaultValue, {}, {}, {}, {}, {},
            policy, capability, advanced, {}};
}

SettingDefinition float_setting(std::string id, std::string category, std::string section,
                                std::string label, double defaultValue, double minimum,
                                double maximum, double step, std::string description,
                                std::uint32_t capability = SettingCapabilityNone,
                                bool advanced = false,
                                SettingApplyPolicy policy = SettingApplyPolicy::Live) {
    return {std::move(id), std::move(category), std::move(section), std::move(label),
            std::move(description), SettingType::Float, defaultValue, minimum, maximum, step,
            {}, {}, policy, capability, advanced, {}};
}

SettingDefinition integer_setting(std::string id, std::string category, std::string section,
                                  std::string label, std::int64_t defaultValue, std::int64_t minimum,
                                  std::int64_t maximum, std::int64_t step, std::string description,
                                  std::uint32_t capability = SettingCapabilityNone,
                                  bool advanced = false,
                                  SettingApplyPolicy policy = SettingApplyPolicy::Live) {
    return {std::move(id), std::move(category), std::move(section), std::move(label),
            std::move(description), SettingType::Integer, defaultValue,
            static_cast<double>(minimum), static_cast<double>(maximum), static_cast<double>(step),
            {}, {}, policy, capability, advanced, {}};
}

SettingDefinition enum_setting(std::string id, std::string category, std::string section,
                               std::string label, std::string defaultValue,
                               std::vector<SettingChoice> choices, std::string description,
                               std::uint32_t capability = SettingCapabilityNone,
                               bool advanced = false,
                               SettingApplyPolicy policy = SettingApplyPolicy::Live) {
    return {std::move(id), std::move(category), std::move(section), std::move(label),
            std::move(description), SettingType::Enum, std::move(defaultValue), {}, {}, {},
            std::move(choices), {}, policy, capability, advanced, {}};
}

void add_all(EditorSettingsRegistry& registry, std::vector<SettingDefinition> definitions) {
    for (SettingDefinition& definition : definitions) {
        const bool added = registry.add(std::move(definition));
        (void)added;
    }
}

} // namespace

std::string setting_value_to_string(const SettingValue& value) {
    return std::visit([](const auto& item) -> std::string {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, bool>) {
            return item ? "On" : "Off";
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            return std::to_string(item);
        } else if constexpr (std::is_same_v<T, double>) {
            std::ostringstream out;
            out << std::setprecision(6) << item;
            return out.str();
        } else {
            return item;
        }
    }, value);
}

std::string setting_scope_name(SettingScope scope) {
    switch (scope) {
        case SettingScope::User: return "User";
        case SettingScope::Project: return "Project";
        case SettingScope::Session: return "Session";
    }
    return "User";
}

bool EditorSettingsRegistry::add(SettingDefinition definition, std::string* error) {
    auto fail = [&](std::string message) { if (error) *error = std::move(message); return false; };
    if (definition.id.empty() || definition.category.empty() || definition.label.empty())
        return fail("setting requires id, category, and label");
    if (find(definition.id)) return fail("duplicate setting id");
    if (!validate_value(definition, definition.defaultValue, error)) return false;
    definitions_.push_back(std::move(definition));
    return true;
}

const SettingDefinition* EditorSettingsRegistry::find(std::string_view id) const noexcept {
    const auto it = std::find_if(definitions_.begin(), definitions_.end(), [&](const SettingDefinition& definition) {
        return definition.id == id;
    });
    return it == definitions_.end() ? nullptr : &*it;
}

std::vector<std::string> EditorSettingsRegistry::categories(std::uint32_t capabilities) const {
    std::vector<std::string> result;
    std::set<std::string, std::less<>> seen;
    for (const SettingDefinition& definition : definitions_) {
        if (capabilities_match(definition, capabilities) && seen.insert(definition.category).second)
            result.push_back(definition.category);
    }
    return result;
}

std::vector<const SettingDefinition*> EditorSettingsRegistry::category(
    std::string_view categoryName, bool includeAdvanced, std::uint32_t capabilities) const {
    std::vector<const SettingDefinition*> result;
    for (const SettingDefinition& definition : definitions_) {
        if (definition.category == categoryName && (includeAdvanced || !definition.advanced) &&
            capabilities_match(definition, capabilities)) result.push_back(&definition);
    }
    std::stable_sort(result.begin(), result.end(), [](const SettingDefinition* a, const SettingDefinition* b) {
        if (a->section != b->section) return a->section < b->section;
        return a->label < b->label;
    });
    return result;
}

std::vector<SettingSearchResult> EditorSettingsRegistry::search(
    std::string_view query, bool includeAdvanced, std::uint32_t capabilities, std::size_t limit) const {
    const std::string requested = lower(query);
    std::vector<SettingSearchResult> result;
    for (const SettingDefinition& definition : definitions_) {
        if ((!includeAdvanced && definition.advanced) || !capabilities_match(definition, capabilities)) continue;
        int score = requested.empty() ? 100 : -1;
        const std::string id = lower(definition.id);
        const std::string label = lower(definition.label);
        const std::string categoryName = lower(definition.category);
        const std::string description = lower(definition.description);
        if (!requested.empty()) {
            if (id == requested || label == requested) score = 0;
            else if (label.starts_with(requested)) score = 10;
            else if (id.starts_with(requested)) score = 20;
            else if (const auto labelPosition = label.find(requested); labelPosition != std::string::npos)
                score = 30 + static_cast<int>(labelPosition);
            else if (const auto categoryPosition = categoryName.find(requested); categoryPosition != std::string::npos)
                score = 50 + static_cast<int>(categoryPosition);
            else if (const auto descriptionPosition = description.find(requested); descriptionPosition != std::string::npos)
                score = 70 + static_cast<int>(descriptionPosition);
            else {
                for (const std::string& keyword : definition.keywords) {
                    if (lower(keyword).find(requested) != std::string::npos) { score = 60; break; }
                }
            }
        }
        if (score < 0) continue;
        SettingScope source{};
        bool inherited{};
        result.push_back({&definition, value(definition.id, &source, &inherited), source, inherited, score});
    }
    std::stable_sort(result.begin(), result.end(), [](const SettingSearchResult& a, const SettingSearchResult& b) {
        if (a.score != b.score) return a.score < b.score;
        if (a.definition->category != b.definition->category) return a.definition->category < b.definition->category;
        return a.definition->label < b.definition->label;
    });
    if (result.size() > limit) result.resize(limit);
    return result;
}

SettingValue EditorSettingsRegistry::value(std::string_view id) const {
    return value(id, nullptr, nullptr);
}

SettingValue EditorSettingsRegistry::value(std::string_view id, SettingScope* sourceScope, bool* inherited) const {
    const std::string key(id);
    if (const auto it = sessionValues_.find(key); it != sessionValues_.end()) {
        if (sourceScope) *sourceScope = SettingScope::Session;
        if (inherited) *inherited = false;
        return it->second;
    }
    if (const auto it = projectValues_.find(key); it != projectValues_.end()) {
        if (sourceScope) *sourceScope = SettingScope::Project;
        if (inherited) *inherited = false;
        return it->second;
    }
    if (const auto it = userValues_.find(key); it != userValues_.end()) {
        if (sourceScope) *sourceScope = SettingScope::User;
        if (inherited) *inherited = false;
        return it->second;
    }
    const SettingDefinition* definition = find(id);
    if (!definition) return false;
    if (sourceScope) *sourceScope = SettingScope::User;
    if (inherited) *inherited = true;
    return definition->defaultValue;
}

SettingValue EditorSettingsRegistry::inherited_value(std::string_view id, SettingScope excludingScope) const {
    const std::string key(id);
    const SettingDefinition* definition = find(id);
    if (!definition) return false;
    if (excludingScope != SettingScope::Session) {
        if (const auto it = sessionValues_.find(key); it != sessionValues_.end()) return it->second;
    }
    if (excludingScope != SettingScope::Project) {
        if (const auto it = projectValues_.find(key); it != projectValues_.end()) return it->second;
    }
    if (excludingScope != SettingScope::User) {
        if (const auto it = userValues_.find(key); it != userValues_.end()) return it->second;
    }
    return definition->defaultValue;
}

bool EditorSettingsRegistry::has_override(SettingScope scope, std::string_view id) const noexcept {
    return layer(scope).find(id) != layer(scope).end();
}

bool EditorSettingsRegistry::set(SettingScope scope, std::string_view id, SettingValue newValue,
                                 std::string* error) {
    const SettingDefinition* definition = find(id);
    if (!definition) { if (error) *error = "unknown setting"; return false; }
    if (!validate_value(*definition, newValue, error)) return false;
    layer(scope)[std::string(id)] = std::move(newValue);
    return true;
}

bool EditorSettingsRegistry::clear(SettingScope scope, std::string_view id) noexcept {
    return layer(scope).erase(std::string(id)) != 0U;
}

void EditorSettingsRegistry::clear_scope(SettingScope scope) noexcept { layer(scope).clear(); }

bool EditorSettingsRegistry::differs_from_default(std::string_view id) const {
    const SettingDefinition* definition = find(id);
    return definition && value(id) != definition->defaultValue;
}

SettingAvailability EditorSettingsRegistry::availability(std::string_view id,std::uint32_t capabilities) const {
    const SettingDefinition* definition=find(id);
    if(!definition) return {false,"Unknown setting."};
    if(!capabilities_match(*definition,capabilities)) return {false,"This option is unavailable in the current build profile."};
    for(const SettingDependency& dependency:definition->dependencies){
        const SettingDefinition* source=find(dependency.settingId);
        if(!source) return {false,"The option has an invalid dependency."};
        if(value(dependency.settingId)!=dependency.requiredValue) return {false,dependency.explanation.empty()?"A required option is disabled.":dependency.explanation};
    }
    return {};
}
bool EditorSettingsRegistry::set_dependencies(std::string_view id,std::vector<SettingDependency> dependencies,std::string* error){
    auto it=std::find_if(definitions_.begin(),definitions_.end(),[&](const SettingDefinition& d){return d.id==id;});
    if(it==definitions_.end()){if(error)*error="unknown dependent setting";return false;}
    for(const auto& dependency:dependencies){if(dependency.settingId==id||!find(dependency.settingId)){if(error)*error="setting dependency is unknown or self-referential";return false;}}
    it->dependencies=std::move(dependencies);return true;
}
std::vector<SettingSearchResult> EditorSettingsRegistry::changed(bool includeAdvanced,std::uint32_t capabilities) const {
    std::vector<SettingSearchResult> result;
    for(const SettingDefinition& definition:definitions_){if((!includeAdvanced&&definition.advanced)||!capabilities_match(definition,capabilities)||!differs_from_default(definition.id))continue;SettingScope source{};bool inherited{};result.push_back({&definition,value(definition.id,&source,&inherited),source,inherited,0});}
    return result;
}
std::size_t EditorSettingsRegistry::reset_category(SettingScope scope,std::string_view categoryName) noexcept {
    std::size_t removed{};auto& values=layer(scope);for(auto it=values.begin();it!=values.end();){const SettingDefinition* definition=find(it->first);if(definition&&definition->category==categoryName){it=values.erase(it);++removed;}else ++it;}return removed;
}

bool EditorSettingsRegistry::validate(std::string* error) const {
    for (const SettingDefinition& definition : definitions_) {
        if (!validate_value(definition, definition.defaultValue, error)) return false;
    }
    for (SettingScope scope : {SettingScope::User, SettingScope::Project, SettingScope::Session}) {
        for (const auto& [id, storedValue] : layer(scope)) {
            const SettingDefinition* definition = find(id);
            if (!definition) { if (error) *error = "setting layer contains unknown id"; return false; }
            if (!validate_value(*definition, storedValue, error)) return false;
        }
    }
    return true;
}

bool EditorSettingsRegistry::validate_value(const SettingDefinition& definition, const SettingValue& value,
                                            std::string* error) const {
    auto fail = [&](std::string message) { if (error) *error = definition.id + ": " + std::move(message); return false; };
    switch (definition.type) {
        case SettingType::Boolean:
            if (!std::holds_alternative<bool>(value)) return fail("expected boolean");
            break;
        case SettingType::Integer: {
            if (!std::holds_alternative<std::int64_t>(value)) return fail("expected integer");
            const double number = static_cast<double>(std::get<std::int64_t>(value));
            if (definition.minimum && number < *definition.minimum) return fail("value is below minimum");
            if (definition.maximum && number > *definition.maximum) return fail("value is above maximum");
            break;
        }
        case SettingType::Float: {
            if (!std::holds_alternative<double>(value)) return fail("expected floating-point value");
            const double number = std::get<double>(value);
            if (!std::isfinite(number)) return fail("value is not finite");
            if (definition.minimum && number < *definition.minimum) return fail("value is below minimum");
            if (definition.maximum && number > *definition.maximum) return fail("value is above maximum");
            break;
        }
        case SettingType::Enum: {
            if (!std::holds_alternative<std::string>(value)) return fail("expected enum string");
            const std::string& selected = std::get<std::string>(value);
            if (std::none_of(definition.choices.begin(), definition.choices.end(), [&](const SettingChoice& choice) {
                    return choice.value == selected;
                })) return fail("enum value is not in the choice list");
            break;
        }
        case SettingType::String:
            if (!std::holds_alternative<std::string>(value)) return fail("expected string");
            if (std::get<std::string>(value).size() > 4096U) return fail("string is too long");
            break;
    }
    return true;
}

std::map<std::string, SettingValue, std::less<>>& EditorSettingsRegistry::layer(SettingScope scope) noexcept {
    if (scope == SettingScope::Project) return projectValues_;
    if (scope == SettingScope::Session) return sessionValues_;
    return userValues_;
}
const std::map<std::string, SettingValue, std::less<>>& EditorSettingsRegistry::layer(SettingScope scope) const noexcept {
    if (scope == SettingScope::Project) return projectValues_;
    if (scope == SettingScope::Session) return sessionValues_;
    return userValues_;
}

std::string EditorSettingsRegistry::serialize_scope(SettingScope scope) const {
    std::ostringstream out;
    out << "DVE_SETTINGS 1 " << setting_scope_name(scope) << '\n';
    const auto& values = layer(scope);
    out << "count " << values.size() << '\n';
    for (const auto& [id, storedValue] : values) {
        const SettingDefinition* definition = find(id);
        if (!definition) continue;
        out << std::quoted(id) << ' ' << type_name(definition->type) << ' ';
        if (const auto* boolean = std::get_if<bool>(&storedValue)) out << (*boolean ? 1 : 0);
        else if (const auto* integer = std::get_if<std::int64_t>(&storedValue)) out << *integer;
        else if (const auto* number = std::get_if<double>(&storedValue)) out << std::setprecision(17) << *number;
        else out << std::quoted(std::get<std::string>(storedValue));
        out << '\n';
    }
    return out.str();
}

bool EditorSettingsRegistry::parse_scope(SettingScope scope, std::string_view text, std::string* error) {
    auto fail = [&](std::string message) { if (error) *error = std::move(message); return false; };
    std::istringstream in{std::string(text)};
    std::string magic, scopeName, key;
    int version{};
    if (!(in >> magic >> version >> scopeName) || magic != "DVE_SETTINGS" || version != 1)
        return fail("unsupported settings format");
    if (scopeName != setting_scope_name(scope)) return fail("settings scope does not match target layer");
    std::size_t count{};
    if (!(in >> key >> count) || key != "count" || count > 100000U) return fail("invalid settings count");
    std::map<std::string, SettingValue, std::less<>> parsed;
    for (std::size_t index = 0; index < count; ++index) {
        std::string id, typeText;
        if (!(in >> std::quoted(id) >> typeText)) return fail("malformed settings entry");
        const SettingDefinition* definition = find(id);
        const auto parsedType = parse_type(typeText);
        if (!definition || !parsedType || *parsedType != definition->type) return fail("settings entry has unknown id or wrong type");
        SettingValue parsedValue;
        if (definition->type == SettingType::Boolean) {
            int value{}; if (!(in >> value) || (value != 0 && value != 1)) return fail("invalid boolean setting");
            parsedValue = value != 0;
        } else if (definition->type == SettingType::Integer) {
            std::int64_t value{}; if (!(in >> value)) return fail("invalid integer setting");
            parsedValue = value;
        } else if (definition->type == SettingType::Float) {
            double value{}; if (!(in >> value)) return fail("invalid floating-point setting");
            parsedValue = value;
        } else {
            std::string value; if (!(in >> std::quoted(value))) return fail("invalid string setting");
            parsedValue = std::move(value);
        }
        if (!validate_value(*definition, parsedValue, error)) return false;
        if (!parsed.emplace(id, std::move(parsedValue)).second) return fail("duplicate setting id in layer");
    }
    in >> std::ws;
    if (!in.eof()) return fail("trailing data in settings file");
    layer(scope) = std::move(parsed);
    return true;
}

std::string EditorSettingsRegistry::serialize_profile(std::string_view profileName,SettingScope scope) const {
    std::ostringstream out;out<<"DVE_SETTINGS_PROFILE 1 "<<std::quoted(std::string(profileName))<<' '<<setting_scope_name(scope)<<'\n';
    const auto& values=layer(scope);out<<"count "<<values.size()<<'\n';
    for(const auto&[id,storedValue]:values){const SettingDefinition* definition=find(id);if(!definition)continue;out<<std::quoted(id)<<' '<<type_name(definition->type)<<' ';if(const auto*b=std::get_if<bool>(&storedValue))out<<(*b?1:0);else if(const auto*i=std::get_if<std::int64_t>(&storedValue))out<<*i;else if(const auto*n=std::get_if<double>(&storedValue))out<<std::setprecision(17)<<*n;else out<<std::quoted(std::get<std::string>(storedValue));out<<'\n';}
    return out.str();
}
bool EditorSettingsRegistry::parse_profile(SettingScope scope,std::string_view text,std::string* error){
    auto fail=[&](std::string m){if(error)*error=std::move(m);return false;};std::istringstream in{std::string(text)};std::string magic,name,scopeName,key;int version{};if(!(in>>magic>>version>>std::quoted(name)>>scopeName)||magic!="DVE_SETTINGS_PROFILE"||version!=1)return fail("unsupported settings profile");if(scopeName!=setting_scope_name(scope))return fail("settings profile scope mismatch");std::size_t count{};if(!(in>>key>>count)||key!="count"||count>100000U)return fail("invalid settings profile count");auto parsed=layer(scope);std::vector<std::string> orphans;
    for(std::size_t index=0;index<count;++index){std::string id,typeText;if(!(in>>std::quoted(id)>>typeText))return fail("malformed settings profile entry");const auto parsedType=parse_type(typeText);SettingValue parsedValue;if(!parsedType)return fail("unknown settings profile type");if(*parsedType==SettingType::Boolean){int v{};if(!(in>>v)||(v!=0&&v!=1))return fail("invalid profile boolean");parsedValue=v!=0;}else if(*parsedType==SettingType::Integer){std::int64_t v{};if(!(in>>v))return fail("invalid profile integer");parsedValue=v;}else if(*parsedType==SettingType::Float){double v{};if(!(in>>v))return fail("invalid profile float");parsedValue=v;}else{std::string v;if(!(in>>std::quoted(v)))return fail("invalid profile string");parsedValue=std::move(v);}const SettingDefinition* definition=find(id);if(!definition){orphans.push_back(id);continue;}if(definition->type!=*parsedType||!validate_value(*definition,parsedValue,error))return false;parsed[id]=std::move(parsedValue);}
    in>>std::ws;if(!in.eof())return fail("trailing settings profile data");layer(scope)=std::move(parsed);orphanedSettings_=std::move(orphans);return true;
}

EditorSettingsRegistry EditorSettingsRegistry::make_default() {
    EditorSettingsRegistry result;
    const std::vector<SettingChoice> onOffQuality{{"low","Low"},{"medium","Medium"},{"high","High"},{"ultra","Ultra"}};
    add_all(result, {
        float_setting("editor.ui_scale","General","Interface","UI Scale",1.0,0.75,3.0,0.05,"Scale editor text and controls."),
        enum_setting("editor.theme","General","Interface","Theme","dark",{{"dark","Dark"},{"light","Light"},{"system","System"}},"Editor appearance."),
        integer_setting("editor.autosave_minutes","General","Files","Autosave Interval",5,1,120,1,"Minutes between recovery saves."),
        boolean_setting("editor.confirm_destructive","General","Safety","Confirm Destructive Actions",true,"Ask before replacing dirty scenes or quitting."),
        boolean_setting("editor.restore_workspace","General","Startup","Restore Workspace",true,"Restore panels, selected tabs, and open assets."),
        boolean_setting("editor.telemetry_local","General","Diagnostics","Collect Local Performance Telemetry",true,"Keep callback, rendering, and task statistics locally."),

        enum_setting("camera.navigation_style","Camera","Navigation","Navigation Style","dve",{{"dve","DVE"},{"unity","Unity"},{"unreal","Unreal"},{"blender","Blender"}},"Mouse and keyboard conventions for viewport navigation."),
        float_setting("camera.fly_speed","Camera","Navigation","Fly Speed",5.0,0.05,1000.0,0.25,"Base editor fly-camera speed in meters per second."),
        float_setting("camera.boost_multiplier","Camera","Navigation","Boost Multiplier",4.0,1.0,20.0,0.25,"Speed multiplier while the boost action is held."),
        float_setting("camera.mouse_sensitivity","Camera","Navigation","Mouse Sensitivity",1.0,0.05,20.0,0.05,"Rotation sensitivity for mouse input."),
        boolean_setting("camera.invert_y","Camera","Navigation","Invert Vertical Look",false,"Invert pitch input."),
        float_setting("camera.orbit_sensitivity","Camera","Navigation","Orbit Sensitivity",1.0,0.05,10.0,0.05,"Orbit rotation sensitivity."),
        float_setting("camera.pan_sensitivity","Camera","Navigation","Pan Sensitivity",1.0,0.05,10.0,0.05,"Viewport pan sensitivity."),
        float_setting("camera.zoom_sensitivity","Camera","Navigation","Zoom Sensitivity",1.0,0.05,10.0,0.05,"Dolly and orthographic zoom sensitivity."),
        enum_setting("camera.default_mode","Camera","Rig","Default Rig","orbit",{{"free","Free Fly"},{"orbit","Orbit"},{"follow","Follow"},{"third_person","Third Person"},{"first_person","First Person"},{"cinematic","Cinematic"}},"Default camera rig for new scenes."),
        enum_setting("camera.default_blend","Camera","Rig","Default Blend","ease_in_out",{{"cut","Cut"},{"linear","Linear"},{"ease_in","Ease In"},{"ease_out","Ease Out"},{"ease_in_out","Ease In Out"},{"smoothstep","Smoothstep"}},"Curve used when changing live camera rigs."),
        float_setting("camera.blend_seconds","Camera","Rig","Default Blend Duration",0.35,0.0,30.0,0.05,"Duration of unspecified camera transitions."),
        boolean_setting("camera.ignore_time_scale","Camera","Rig","Ignore Time Scale",false,"Keep input, damping, and blends responsive during slow motion."),
        float_setting("camera.position_damping","Camera","Follow","Position Damping",0.12,0.0,10.0,0.01,"Time constant for follow-position smoothing."),
        float_setting("camera.aim_damping","Camera","Follow","Aim Damping",0.08,0.0,10.0,0.01,"Time constant for look-at smoothing."),
        float_setting("camera.look_ahead","Camera","Follow","Look Ahead",0.15,0.0,5.0,0.01,"Seconds of target velocity used for predictive framing."),
        float_setting("camera.dead_zone","Camera","Follow","Dead Zone",0.02,0.0,1.0,0.01,"Screen-space region that does not move the camera."),
        float_setting("camera.soft_zone","Camera","Follow","Soft Zone",0.65,0.0,1.0,0.01,"Screen-space region where target motion is damped."),
        boolean_setting("camera.collision","Camera","Collision","Collision Avoidance",true,"Sweep a camera probe between the target and ideal camera position."),
        float_setting("camera.collision_radius","Camera","Collision","Probe Radius",0.22,0.0,10.0,0.01,"Sphere radius used for camera collision checks."),
        float_setting("camera.collision_recovery","Camera","Collision","Occlusion Recovery",0.25,0.0,10.0,0.01,"Time used to return to the ideal camera distance."),
        boolean_setting("camera.preserve_line_of_sight","Camera","Collision","Preserve Line of Sight",true,"Prefer positions that keep the tracked subject visible."),
        enum_setting("camera.projection","Camera","Lens","Projection","perspective",{{"perspective","Perspective"},{"orthographic","Orthographic"}},"Default projection mode."),
        float_setting("camera.field_of_view","Camera","Lens","Vertical Field of View",60.0,1.0,179.0,1.0,"Vertical perspective field of view in degrees."),
        float_setting("camera.near_plane","Camera","Lens","Near Clip",0.05,0.001,100.0,0.001,"Near clipping distance in meters.",SettingCapabilityNone,true),
        float_setting("camera.far_plane","Camera","Lens","Far Clip",5000.0,1.0,10000000.0,10.0,"Far clipping distance in meters.",SettingCapabilityNone,true),
        boolean_setting("camera.physical_lens","Camera","Physical Lens","Use Physical Lens",false,"Derive field of view from focal length and sensor size."),
        float_setting("camera.focal_length_mm","Camera","Physical Lens","Focal Length",35.0,1.0,2000.0,1.0,"Physical lens focal length in millimeters."),
        enum_setting("camera.sensor_preset","Camera","Physical Lens","Filmback","full_frame",{{"super16","Super 16"},{"super35","Super 35"},{"full_frame","Full Frame 35"},{"anamorphic35","Anamorphic 35"},{"imax15","IMAX 15-perf"},{"imax_digital","IMAX Digital"},{"custom","Custom"}},"Select a Super 16, Super 35, full-frame, anamorphic, or IMAX filmback preset."),
        enum_setting("camera.gate_fit","Camera","Physical Lens","Gate Fit","vertical",{{"vertical","Vertical"},{"horizontal","Horizontal"},{"fill","Fill"},{"overscan","Overscan"},{"stretch","Stretch"}},"How the sensor gate fits the viewport."),
        float_setting("camera.aperture","Camera","Physical Lens","Aperture",2.8,0.5,64.0,0.1,"Physical aperture f-stop."),
        float_setting("camera.focus_distance","Camera","Physical Lens","Focus Distance",10.0,0.01,1000000.0,0.1,"Focus plane distance in meters."),
        boolean_setting("camera.preview_selected","Camera","Preview","Preview Selected Cameras",true,"Show picture-in-picture views for selected camera rigs."),
        boolean_setting("camera.preview_cinematic_effects","Camera","Preview","Preview Cinematic Effects",true,"Apply the selected camera cinematic profile in editor preview."),
        boolean_setting("camera.lock_viewport_to_selected","Camera","Preview","Lock Viewport to Selected Camera",false,"Keep the editor viewport synchronized with the selected camera rig."),
        float_setting("camera.preview_size","Camera","Preview","Preview Size",0.25,0.1,0.75,0.05,"Picture-in-picture size relative to the viewport."),
        boolean_setting("camera.show_frustum","Camera","Preview","Show Frustum",true,"Draw selected camera frustums in the editor."),
        boolean_setting("camera.allow_shakes_in_editor","Camera","Impulse","Preview Camera Shakes",true,"Allow authored camera impulses and shakes in editor preview."),
        float_setting("camera.shake_scale","Camera","Impulse","Shake Scale",1.0,0.0,5.0,0.05,"Global camera-shake intensity."),

        boolean_setting("viewport.grid","Viewport","Overlays","Grid",true,"Display the world grid."),
        boolean_setting("viewport.anchors","Viewport","Overlays","Anchors",true,"Display structural anchors."),
        boolean_setting("viewport.bounds","Viewport","Overlays","Object Bounds",true,"Display object bounds."),
        boolean_setting("viewport.collision","Viewport","Overlays","Collision Shapes",false,"Display collision proxies."),
        boolean_setting("viewport.xray_selection","Viewport","Overlays","X-Ray Selection",false,"Draw selected objects through occluders."),
        boolean_setting("viewport.statistics","Viewport","Overlays","Statistics",false,"Display camera, render, and scene statistics."),
        boolean_setting("viewport.safe_frames","Viewport","Camera","Safe Frames",false,"Display filmback and aspect-ratio guides."),
        enum_setting("viewport.shading_mode","Viewport","Shading","Shading Mode","lit",{{"lit","Lit"},{"unlit","Unlit"},{"wireframe","Wireframe"},{"material_ids","Material IDs"},{"normals","Normals/Tangents"},{"collision","Collision"},{"voxel_debug","Voxel Debug"},{"lighting_only","Lighting Only"},{"overdraw","Overdraw"}},"Viewport visualization mode."),
        boolean_setting("viewport.game_view","Viewport","Overlays","Game View",false,"Hide editor-only overlays in the viewport."),
        boolean_setting("viewport.realtime","Viewport","Rendering","Realtime Updates",true,"Continuously update the active viewport."),
        boolean_setting("viewport.immersive","Viewport","Layout","Immersive Viewport",false,"Maximize the active viewport while preserving the workspace layout."),
        integer_setting("viewport.max_voxels","Viewport","Performance","Maximum Draw Voxels",120000,1000,5000000,1000,"Maximum voxel markers drawn by the editor.",SettingCapabilityVoxel),
        float_setting("viewport.grid_spacing","Viewport","Grid","Grid Spacing",1.0,0.001,10000.0,0.05,"World-grid spacing in meters."),

        enum_setting("render.backend","Rendering","Backend","Graphics Backend","vulkan",{{"vulkan","Vulkan"},{"d3d12","Direct3D 12"},{"metal","Metal"},{"reference","CPU Reference"}},"Preferred rendering backend.",SettingCapabilityNone,false,SettingApplyPolicy::RestartRequired),
        enum_setting("render.display_mode","Rendering","Display","Display Mode","windowed",{{"windowed","Windowed"},{"borderless","Borderless Fullscreen"},{"exclusive","Exclusive Fullscreen"}},"Choose how the game window occupies the selected display."),
        enum_setting("render.resolution_profile","Rendering","Display","Resolution Profile","fhd_1080p",{{"hd_720p","HD 1280x720"},{"fhd_1080p","Full HD 1920x1080"},{"qhd_1440p","QHD 2560x1440"},{"uhd_4k","4K UHD 3840x2160"},{"uwqhd","Ultrawide QHD 3440x1440"},{"portrait_fhd","Portrait Full HD 1080x1920"},{"custom","Custom"}},"Select a tested output-resolution profile."),
        integer_setting("render.preferred_display","Rendering","Display","Preferred Display",0,0,64,1,"Stable project-local display slot; zero follows the operating-system primary display."),
        boolean_setting("render.high_dpi","Rendering","Display","High-DPI Drawable",true,"Render using drawable pixels while mapping pointer input from logical window coordinates."),
        boolean_setting("render.vsync","Rendering","Display","Vertical Synchronization",true,"Synchronize presentation to display refresh."),
        integer_setting("render.frame_limit","Rendering","Display","Frame Limit",0,0,1000,1,"Maximum frames per second; zero is unlimited."),
        float_setting("render.resolution_scale","Rendering","Display","Resolution Scale",1.0,0.25,2.0,0.05,"Internal rendering resolution multiplier."),
        integer_setting("render.local_players","Rendering","Multi-Viewport","Local Players",1,1,4,1,"Number of local player viewports."),
        enum_setting("render.split_screen_layout","Rendering","Multi-Viewport","Split-Screen Layout","auto",{{"auto","Automatic"},{"single","Single Shared Camera"},{"horizontal","Horizontal Two Player"},{"vertical","Vertical Two Player"},{"three-left","Three Player Main Left"},{"three-top","Three Player Main Top"},{"quad","Four Player Grid"},{"picture-in-picture","Picture in Picture"}},"Select the per-window viewport arrangement."),
        boolean_setting("render.dynamic_shared_camera","Rendering","Multi-Viewport","Dynamic Shared Camera",true,"Merge nearby two-player views and split them with hysteresis when players separate."),
        boolean_setting("render.spectator_window","Rendering","Multi-Window","Spectator Window",false,"Request an independent spectator or diagnostics presentation surface."),
        boolean_setting("render.hdr","Rendering","Color","HDR Output",false,"Use HDR presentation when supported.",SettingCapabilityNone,false,SettingApplyPolicy::RestartRequired),
        float_setting("render.exposure","Rendering","Color","Exposure",1.0,0.01,32.0,0.05,"Global linear exposure multiplier."),
        enum_setting("render.tonemap","Rendering","Color","Tonemap","aces",{{"aces","ACES Fitted"},{"reinhard","Reinhard"},{"clamp","Clamp"}},"Display-referred tonemap operator."),
        boolean_setting("render.bloom","Rendering","Post Processing","Bloom",true,"Enable soft-knee HDR bloom."),
        float_setting("render.bloom_threshold","Rendering","Post Processing","Bloom Threshold",1.0,0.0,100.0,0.05,"Linear-light bloom threshold."),
        enum_setting("render.texture_filter","Rendering","Textures","Texture Filtering","trilinear",{{"nearest","Nearest"},{"bilinear","Bilinear"},{"trilinear","Trilinear"},{"anisotropic","Anisotropic"}},"Texture sampling filter."),
        integer_setting("render.anisotropy","Rendering","Textures","Anisotropy",8,1,16,1,"Maximum anisotropic filtering level."),
        integer_setting("render.texture_budget_mb","Rendering","Textures","Texture Budget",1024,64,32768,64,"Resident texture memory budget in MiB.",SettingCapabilityPolygon),
        enum_setting("render.gi_mode","Rendering","Lighting","Global Illumination","voxel_one_bounce",{{"off","Off"},{"ambient","Ambient Hemisphere"},{"voxel_one_bounce","Voxel One Bounce"}},"Indirect-lighting method; enabled by default for new projects."),
        enum_setting("render.gi_quality","Rendering","Lighting","GI Quality","medium",onOffQuality,"One-bounce sample count and temporal quality."),
        float_setting("render.gi_intensity","Rendering","Lighting","GI Intensity",0.65,0.0,4.0,0.05,"Diffuse indirect-light contribution."),
        float_setting("render.gi_distance","Rendering","Lighting","GI Distance",12.0,0.1,250.0,0.5,"Maximum one-bounce trace distance in meters."),
        enum_setting("render.shadow_mode","Rendering","Lighting","Shadow Type","soft",{{"off","Off"},{"hard","Hard Ray-Traced"},{"soft","Soft Area"},{"contact","Contact"},{"hybrid","Soft + Contact"}},"Directional shadow method."),
        enum_setting("render.shadow_quality","Rendering","Lighting","Shadow Quality","high",onOffQuality,"Directional shadow sample count."),
        float_setting("render.shadow_strength","Rendering","Lighting","Shadow Strength",0.85,0.0,1.0,0.05,"Opacity of fully occluded directional shadows."),
        float_setting("render.shadow_softness","Rendering","Lighting","Sun Angular Radius",0.012,0.0,0.25,0.001,"Angular radius in radians used by soft shadows."),
        float_setting("render.contact_shadow_distance","Rendering","Lighting","Contact Shadow Distance",2.0,0.05,50.0,0.05,"Maximum distance for contact-shadow blockers."),
        enum_setting("render.ao_quality","Rendering","Lighting","Ambient Occlusion","medium",onOffQuality,"Geometric ambient-occlusion quality."),
        integer_setting("render.translucent_layers","Rendering","Transparency","Translucent Layers",4,1,16,1,"Maximum continuation layers for voxel transparency.",SettingCapabilityVoxel),
        boolean_setting("render.gabor.enabled","Rendering","Gabor Volumes","Enable Gabor Fields",true,"Enable rendering for imported .dgabor volume assets."),
        enum_setting("render.gabor.mode","Rendering","Gabor Volumes","Rendering Mode","emission_absorption",{{"absorption","Absorption Preview"},{"emission_absorption","Emission-Absorption"},{"scattering","Scattering [Experimental]"}},"Gabor volume integration method."),
        enum_setting("render.gabor.quality","Rendering","Gabor Volumes","Quality","medium",{{"low","Low"},{"medium","Medium"},{"high","High"},{"cinematic","Cinematic"}},"Ray-step and primitive-budget preset."),
        boolean_setting("render.gabor.continuous_lod","Rendering","Gabor Volumes","Continuous LOD",true,"Fade frequency bands according to projected footprint."),
        boolean_setting("render.gabor.temporal_accumulation","Rendering","Gabor Volumes","Temporal Accumulation",true,"Accumulate bounded volumetric history when the GPU path is active."),
        boolean_setting("render.gabor.cast_shadows","Rendering","Gabor Volumes","Cast Volume Shadows",true,"Allow Gabor volumes to attenuate scene lighting."),
        boolean_setting("render.gabor.receive_shadows","Rendering","Gabor Volumes","Receive Scene Shadows",true,"Apply scene shadowing inside the volume."),
        integer_setting("render.gabor.max_primitives_per_tile","Rendering","Gabor Volumes","Maximum Primitives per Tile",512,32,4096,32,"Bound per-tile Gabor work and report overflow.",SettingCapabilityNone,true),
        float_setting("render.gabor.lod_bias","Rendering","Gabor Volumes","LOD Bias",0.0,-4.0,4.0,0.1,"Bias continuous Gabor frequency-level selection."),

        enum_setting("geometry.mode","Geometry","Build Profile","Geometry Mode","hybrid",{{"voxel","Voxel"},{"polygon","Polygon"},{"hybrid","Hybrid"}},"Geometry systems included in packaged builds.",SettingCapabilityNone,false,SettingApplyPolicy::RestartRequired),
        integer_setting("voxel.brick_budget","Voxel","Streaming","Resident Brick Budget",262144,1024,16777216,1024,"Maximum resident voxel bricks.",SettingCapabilityVoxel),
        enum_setting("voxel.destruction_quality","Voxel","Destruction","Destruction Quality","high",onOffQuality,"Fracture, debris, and connectivity detail.",SettingCapabilityVoxel),
        integer_setting("voxel.debris_limit","Voxel","Destruction","Debris Limit",2048,0,1000000,64,"Maximum active voxel debris bodies.",SettingCapabilityVoxel),
        boolean_setting("voxel.async_connectivity","Voxel","Simulation","Async Connectivity",true,"Move connectivity and fracture discovery to workers.",SettingCapabilityVoxel),
        float_setting("voxel.ray_step_scale","Voxel","Rendering","Ray Step Scale",1.0,0.25,4.0,0.05,"Voxel ray-march step multiplier.",SettingCapabilityVoxel,true),
        enum_setting("voxel.material_mode","Voxel","Material Representation","Voxel Material Mode","hybrid",{{"baked","Baked Properties"},{"single","Single Material ID"},{"deferred","Deferred Brick Palette"},{"hybrid","Hybrid"}},"Choose conventional baking, one material ID, deferred brick palettes, or automatic hybrid selection.",SettingCapabilityVoxel),
        enum_setting("voxel.material_platform","Voxel","Material Representation","Material Platform Profile","high_end_desktop",{{"editor","Editor"},{"high_end_desktop","High-End Desktop"},{"console","Console"},{"integrated_gpu","Integrated GPU"},{"mobile","Mobile"},{"server","Dedicated Server"}},"Platform policy used when selecting cooked voxel-material representations.",SettingCapabilityVoxel),
        enum_setting("voxel.material_overflow","Voxel","Material Representation","Palette Overflow","bake",{{"dominant","Use Dominant Material"},{"bake","Bake Conventional Properties"},{"recook","Request Recook"},{"reject","Reject Brick"}},"Response when more source materials meet than the brick palette can encode.",SettingCapabilityVoxel),
        float_setting("voxel.material_deferred_distance","Voxel","Material Representation","Deferred Maximum Distance",40.0,0.0,10000.0,1.0,"Maximum camera distance for deferred palette blending in hybrid mode.",SettingCapabilityVoxel),
        integer_setting("voxel.material_deferred_lod","Voxel","Material Representation","Deferred Maximum LOD",1,0,16,1,"Coarsest LOD allowed to retain deferred material identity.",SettingCapabilityVoxel),
        integer_setting("voxel.material_palette_slots","Voxel","Material Representation","Maximum Palette Slots",4,1,4,1,"Maximum source materials retained by a deferred brick palette.",SettingCapabilityVoxel),
        boolean_setting("voxel.material_retain_baked","Voxel","Cooked Fallbacks","Retain Baked Fallback",true,"Cook a conventional material fallback for runtime switching and distant LODs.",SettingCapabilityVoxel),
        boolean_setting("voxel.material_retain_single","Voxel","Cooked Fallbacks","Retain Single-Material Fallback",true,"Cook a dominant-material fallback for constrained platforms and servers.",SettingCapabilityVoxel),
        boolean_setting("voxel.material_runtime_switching","Voxel","Cooked Fallbacks","Runtime Switching",true,"Allow the renderer to change material representation without invalid surfaces when compatible cooked data exists.",SettingCapabilityVoxel),
        boolean_setting("voxel.material_four_way","Voxel","Material Representation","Four-Way Palette Blending",true,"Allow four complete material stacks on eligible hero bricks.",SettingCapabilityVoxel,true),
        boolean_setting("voxel.material_prefer_destructible","Voxel","Material Representation","Prefer Deferred for Destruction",true,"Retain material identity near fresh fractures and destructible hero surfaces.",SettingCapabilityVoxel),
        boolean_setting("voxel.material_memory_fallback","Voxel","Material Representation","Fallback under Memory Pressure",true,"Prefer baked properties when texture or representation residency is constrained.",SettingCapabilityVoxel),

        float_setting("polygon.lod_bias","Polygon","LOD","LOD Bias",0.0,-4.0,4.0,0.1,"Screen-space LOD selection bias.",SettingCapabilityPolygon),
        boolean_setting("polygon.instancing","Polygon","Rendering","GPU Instancing",true,"Batch identical meshes with instance records.",SettingCapabilityPolygon),
        boolean_setting("polygon.frustum_culling","Polygon","Rendering","Frustum Culling",true,"Cull meshes outside the camera frustum.",SettingCapabilityPolygon),
        boolean_setting("polygon.occlusion_culling","Polygon","Rendering","Occlusion Culling",false,"Cull meshes hidden by depth or hierarchy data.",SettingCapabilityPolygon,true),
        enum_setting("polygon.collision","Polygon","Collision","Collision Cooking","bvh",{{"bounds","Bounds"},{"bvh","Triangle BVH"},{"convex","Convex"},{"jolt_mesh","Jolt Mesh"}},"Polygon collision representation.",SettingCapabilityPolygon),
        boolean_setting("polygon.mesh_streaming","Polygon","Streaming","Mesh Streaming",true,"Stream cooked mesh heaps in the background.",SettingCapabilityPolygon),

        boolean_setting("material.global_parameters","Materials","Global Parameters","Material Parameter Collections",true,"Publish project-wide scalar and vector parameters."),
        integer_setting("material.layer_limit","Materials","Layers","Layer Limit",4,1,8,1,"Maximum flattened material layers."),
        boolean_setting("material.clear_coat","Materials","Shading Models","Clear Coat",true,"Enable the two-lobe clear-coat shading model."),
        boolean_setting("material.foliage","Materials","Shading Models","Two-Sided Foliage",true,"Enable thin-surface foliage transmission."),
        boolean_setting("material.subsurface","Materials","Shading Models","Subsurface",true,"Enable voxel-thickness subsurface transmission."),
        boolean_setting("material.validate_gpu_layout","Materials","Validation","Validate GPU Layout",true,"Check CPU/HLSL record offsets and identifiers."),

        float_setting("audio.master_gain_db","Audio","Mixer","Master Gain",0.0,-80.0,12.0,0.5,"Master output gain in decibels.",SettingCapabilityAudio),
        float_setting("audio.music_gain_db","Audio","Mixer","Music Gain",0.0,-80.0,12.0,0.5,"Music bus gain in decibels.",SettingCapabilityAudio),
        float_setting("audio.effects_gain_db","Audio","Mixer","Effects Gain",0.0,-80.0,12.0,0.5,"Effects bus gain in decibels.",SettingCapabilityAudio),
        float_setting("audio.dialogue_gain_db","Audio","Mixer","Dialogue Gain",0.0,-80.0,12.0,0.5,"Dialogue bus gain in decibels.",SettingCapabilityAudio),
        integer_setting("audio.sample_rate","Audio","Device","Sample Rate",48000,8000,384000,1000,"Preferred device and project sample rate.",SettingCapabilityAudio,false,SettingApplyPolicy::RestartRequired),
        integer_setting("audio.buffer_frames","Audio","Device","Buffer Frames",256,32,4096,32,"Requested hardware callback block size.",SettingCapabilityAudio,false,SettingApplyPolicy::RestartRequired),
        enum_setting("audio.spatializer","Audio","Spatial","Spatializer","native",{{"native","DVE Native"},{"steam_audio","Steam Audio"},{"none","None"}},"Spatial audio backend.",SettingCapabilityAudio),
        boolean_setting("audio.hrtf","Audio","Spatial","HRTF",true,"Use binaural head-related transfer functions.",SettingCapabilityAudio),
        enum_setting("audio.granular_quality","Audio","Synthesis","Granular Quality","high",onOffQuality,"Maximum grain admission and interpolation quality.",SettingCapabilityAudio),
        integer_setting("audio.stream_preload_ms","Audio","Streaming","Stream Preload",250,0,10000,10,"Audio stream look-ahead in milliseconds.",SettingCapabilityAudio),
        boolean_setting("audio.loudness_normalization","Audio","Output","Loudness Normalization",false,"Apply project loudness targets during export.",SettingCapabilityAudio),

        enum_setting("input.shortcut_profile","Input","Shortcuts","Shortcut Profile","dve",{{"dve","DVE Default"},{"unity","Unity Familiar"},{"unreal","Unreal Familiar"},{"accessible","Accessibility One-Handed"},{"custom","Blank Custom"}},"Active context-aware keyboard and mouse shortcut profile."),
        boolean_setting("input.show_shortcuts_in_menus","Input","Shortcuts","Show Shortcuts in Menus",true,"Display the active primary binding beside menu commands."),
        boolean_setting("input.warn_shortcut_conflicts","Input","Shortcuts","Warn About Shortcut Conflicts",true,"Require an explicit override when assigning a binding used by an overlapping context."),
        enum_setting("input.mouse4_viewport","Input","Mouse","Mouse4 Viewport Action","profile",{{"profile","Use Shortcut Profile"},{"previous_view","Previous Camera Position"},{"sample_material","Sample Material"},{"none","Unassigned"}},"Optional viewport-only override for Mouse4. Use Shortcut Profile keeps the context-aware binding registry authoritative."),
        integer_setting("input.double_click_ms","Input","Mouse","Double-Click Interval",400,100,1000,10,"Maximum interval between pointer presses that counts as a double-click."),
        integer_setting("input.double_click_distance_px","Input","Mouse","Double-Click Distance",5,0,32,1,"Maximum pointer movement between presses that counts as a double-click."),
        enum_setting("input.gamepad_prompts","Input","UI","Prompt Device","automatic",{{"automatic","Automatic"},{"keyboard","Keyboard/Mouse"},{"xbox","Xbox"},{"playstation","PlayStation"},{"switch","Nintendo"}},"Glyph family used for bound actions."),
        boolean_setting("input.raw_mouse","Input","Mouse","Raw Mouse Input",true,"Bypass operating-system pointer acceleration."),
        float_setting("input.controller_dead_zone","Input","Controller","Controller Dead Zone",0.15,0.0,0.95,0.01,"Radial analog-stick dead zone."),
        float_setting("input.ui_repeat_delay","Input","UI","Navigation Repeat Delay",0.35,0.0,2.0,0.01,"Delay before held UI navigation repeats."),
        float_setting("input.ui_repeat_rate","Input","UI","Navigation Repeat Rate",0.08,0.01,1.0,0.01,"Interval between repeated UI navigation events."),

        float_setting("physics.fixed_timestep","Physics","Simulation","Fixed Timestep",0.0166666667,0.001,0.1,0.001,"Authoritative physics step duration."),
        integer_setting("physics.max_substeps","Physics","Simulation","Maximum Substeps",4,1,32,1,"Maximum catch-up simulation steps per frame."),
        float_setting("physics.gravity","Physics","World","Gravity",-9.81,-1000.0,1000.0,0.01,"World vertical acceleration in m/s²."),
        boolean_setting("physics.continuous_collision","Physics","Collision","Continuous Collision",true,"Use continuous collision for fast bodies."),
        enum_setting("physics.backend","Physics","Backend","3D Physics Backend","automatic",{{"automatic","Automatic"},{"jolt","Jolt"},{"box3d","Box3D"},{"reference","Reference"}},"Production 3D rigid-body backend. Automatic prefers Jolt, then Box3D, then the reference adapter.",SettingCapabilityNone,false,SettingApplyPolicy::RestartRequired),
        integer_setting("physics.worker_threads","Physics","Solver","Worker Threads",0,0,31,1,"Zero selects the backend's conservative automatic worker policy.",SettingCapabilityNone,false,SettingApplyPolicy::RestartRequired),
        integer_setting("physics.substeps","Physics","Solver","Collision Substeps",1,1,8,1,"Collision solver substeps per fixed update.",SettingCapabilityNone),
        integer_setting("physics.velocity_iterations","Physics","Jolt","Velocity Iterations",10,2,64,1,"Jolt velocity-constraint iterations.",SettingCapabilityJolt),
        integer_setting("physics.position_iterations","Physics","Jolt","Position Iterations",2,1,32,1,"Jolt position-correction iterations.",SettingCapabilityJolt),
        boolean_setting("physics.deterministic","Physics","Solver","Deterministic Simulation",true,"Use deterministic solver ordering where supported."),
        boolean_setting("physics.allow_sleeping","Physics","Solver","Allow Sleeping",true,"Allow settled rigid bodies to sleep."),

        boolean_setting("scripting.lua","Scripting","Runtime","Lua Gameplay Scripting",true,"Enable the Lua gameplay host.",SettingCapabilityLua,false,SettingApplyPolicy::RestartRequired),
        boolean_setting("scripting.hot_reload","Scripting","Runtime","Hot Reload",true,"Reload scripts transactionally with state migration.",SettingCapabilityLua),
        integer_setting("scripting.migration_timeout_ms","Scripting","Runtime","Migration Timeout",50,1,10000,1,"Maximum state-migration time per script.",SettingCapabilityLua,true),
        boolean_setting("scripting.strict_errors","Scripting","Validation","Strict Script Errors",true,"Fail scene validation for unresolved script errors.",SettingCapabilityLua),

        enum_setting("build.configuration","Build","Target","Configuration","release",{{"debug","Debug"},{"release","Release"},{"distribution","Distribution"}},"Compiler and packaging configuration.",SettingCapabilityNone,false,SettingApplyPolicy::RestartRequired),
        enum_setting("build.target","Build","Target","Platform","host",{{"host","Current Host"},{"windows","Windows"},{"linux","Linux"},{"macos","macOS"}},"Package target platform."),
        boolean_setting("build.sanitizers","Build","Validation","Sanitizers",false,"Enable address and undefined-behavior sanitizers.",SettingCapabilityNone,false,SettingApplyPolicy::RestartRequired),
        boolean_setting("build.headless","Build","Target","Headless",false,"Exclude windowing and graphics presentation."),
        boolean_setting("build.deterministic_oracles","Build","Validation","Deterministic Oracles",true,"Run reproducible render and audio checks during packaging."),
        boolean_setting("build.verify_dependencies","Build","Validation","Verify Dependency Hashes",true,"Reject stale or missing asset dependencies."),

        boolean_setting("accessibility.high_contrast","Accessibility","Vision","High Contrast",false,"Increase interface and diagnostic contrast."),
        enum_setting("accessibility.color_vision","Accessibility","Vision","Color Vision Mode","standard",{{"standard","Standard"},{"deuteranopia","Deuteranopia"},{"protanopia","Protanopia"},{"tritanopia","Tritanopia"}},"Diagnostic and editor color adaptation."),
        boolean_setting("accessibility.reduced_motion","Accessibility","Motion","Reduced Motion",false,"Reduce camera shakes, menu transitions, and animated effects."),
        float_setting("accessibility.camera_shake","Accessibility","Motion","Camera Shake",1.0,0.0,1.0,0.05,"Player camera-shake multiplier."),
        boolean_setting("accessibility.subtitles","Accessibility","Audio","Subtitles",true,"Display dialogue and authored audio captions."),
        enum_setting("accessibility.dynamic_range","Accessibility","Audio","Dynamic Range","full",{{"night","Night"},{"medium","Medium"},{"full","Full"}},"Output compressor and loudness profile."),
        boolean_setting("accessibility.mono_audio","Accessibility","Audio","Mono Audio",false,"Downmix output for single-channel listening."),
        boolean_setting("accessibility.focus_indicators","Accessibility","Navigation","Strong Focus Indicators",true,"Draw clear keyboard/controller focus outlines."),

        boolean_setting("camera.horizon_lock","Camera","Accessibility","Horizon Lock",false,"Keep the camera up vector aligned to the world horizon."),
        float_setting("camera.input_acceleration","Camera","Navigation","Input Acceleration",0.0,0.0,20.0,0.05,"Gradually accelerate continuous camera movement."),
        float_setting("camera.input_smoothing","Camera","Navigation","Input Smoothing",0.0,0.0,2.0,0.01,"Low-pass filter camera look and movement input."),
        enum_setting("camera.split_screen_layout","Camera","Outputs","Split-Screen Layout","single",{{"single","Single View"},{"vertical","Vertical Split"},{"horizontal","Horizontal Split"},{"quad","Four Views"}},"Default runtime viewport layout."),
        boolean_setting("camera.render_target_outputs","Camera","Outputs","Render-to-Texture Cameras",true,"Allow camera viewports to publish named offscreen outputs."),
        boolean_setting("camera.temporal_reset_on_cut","Camera","Rendering","Reset Temporal History on Cuts",true,"Reset TAA and motion history after camera cuts."),
        boolean_setting("camera.depth_of_field","Camera","Rendering","Physical Depth of Field",true,"Use physical lens, aperture, and focus distance for depth of field."),
        boolean_setting("camera.show_focus_planes","Camera","Preview","Show Focus Planes",false,"Draw near, focus, and far depth-of-field guides."),
        boolean_setting("camera.constant_speed_dolly","Camera","Sequencer","Constant-Speed Dolly",false,"Reparameterize camera splines by arc length."),
        float_setting("camera.sequence_scrub_rate","Camera","Sequencer","Scrub Rate",1.0,0.05,8.0,0.05,"Timeline playback speed while previewing camera sequences."),
        enum_setting("camera.default_cinematic_preset","Camera","Cinematic Defaults","Default Cinematic Preset","neutral",{{"neutral","Neutral"},{"academy","Academy Classic"},{"imax143","IMAX 1.43"},{"imax190","IMAX 1.90"},{"scope239","Scope 2.39"},{"anamorphic","Vintage Anamorphic"},{"fisheye","Fisheye Action"},{"split_diopter","Split Diopter"},{"bleach_bypass","Bleach Bypass"},{"seventies","Seventies Film"}},"Initial cinematic profile for newly created camera rigs."),
        enum_setting("camera.dof_quality","Camera","Cinematic Quality","Depth of Field Quality","high",{{"off","Off"},{"low","Low"},{"medium","Medium"},{"high","High"},{"cinematic","Cinematic"}},"Aperture-shaped depth-of-field sampling quality."),
        enum_setting("camera.motion_blur_quality","Camera","Cinematic Quality","Motion Blur Quality","high",onOffQuality,"Motion-vector blur sampling quality."),
        enum_setting("camera.lens_effect_quality","Camera","Cinematic Quality","Lens Effects Quality","high",onOffQuality,"Distortion, fisheye, aberration, flare, and halation quality."),
        enum_setting("camera.film_grain_quality","Camera","Cinematic Quality","Film Grain Quality","medium",onOffQuality,"Film-grain evaluation quality."),
        integer_setting("camera.lut_resolution","Camera","Color Pipeline","3D LUT Resolution",32,16,64,16,"Preferred resident LUT cube edge length."),
        enum_setting("camera.lut_streaming","Camera","Color Pipeline","LUT Streaming","on_demand",{{"resident","Always Resident"},{"on_demand","On Demand"},{"disabled","Disabled"}},"Color-grading LUT residency policy."),
        enum_setting("camera.tone_map_output","Camera","Color Pipeline","Output Grading","hdr_auto",{{"sdr","SDR"},{"hdr_auto","Automatic HDR/SDR"},{"hdr10","HDR10"}},"Project output transform used by camera grading."),
        enum_setting("camera.missing_lut_fallback","Camera","Color Pipeline","Missing LUT Fallback","neutral",{{"neutral","Neutral Grade"},{"last_resident","Last Resident LUT"},{"disable_grade","Disable LUT Grade"}},"Behavior when a referenced LUT is unavailable."),
        boolean_setting("camera.accessibility_reduce_flare","Camera","Accessibility","Reduce Lens Flare",false,"Constrain anamorphic flare and halation in photosensitive modes."),
        boolean_setting("camera.accessibility_reduce_blur","Camera","Accessibility","Reduce Motion Blur",false,"Constrain shutter blur and rapid camera smear."),
        boolean_setting("camera.accessibility_disable_grain","Camera","Accessibility","Disable Grain and Gate Weave",false,"Remove film grain and gate weave from camera output."),
        boolean_setting("camera.accessibility_limit_fisheye","Camera","Accessibility","Limit Fisheye Distortion",false,"Clamp high-distortion fisheye presets."),

        boolean_setting("diagnostics.render_stats","Diagnostics","Overlays","Render Statistics",false,"Show frame, draw, residency, and camera statistics."),
        boolean_setting("diagnostics.audio_stats","Diagnostics","Overlays","Audio Statistics",false,"Show callback, voice, grain, and stream telemetry."),
        boolean_setting("diagnostics.camera_debug","Diagnostics","Overlays","Camera Debug",false,"Show live rig, blend, collision, and shake telemetry."),
        boolean_setting("diagnostics.validation_on_save","Diagnostics","Validation","Validate on Save",true,"Run scene and asset validation before committing files."),
        boolean_setting("diagnostics.gpu_markers","Diagnostics","GPU","GPU Debug Markers",true,"Emit named render-pass and resource markers.",SettingCapabilityVulkan),
        boolean_setting("diagnostics.capture_repro","Diagnostics","Capture","Reproduction Capture",false,"Record deterministic inputs and state snapshots."),

        boolean_setting("plugins.clap_host","Plugins","Audio","CLAP Host",true,"Enable CLAP discovery and hosting.",SettingCapabilityAudio),
        boolean_setting("plugins.clap_sandbox","Plugins","Audio","Sandbox Audio Plugins",true,"Run third-party audio plugins out of process.",SettingCapabilityAudio,true),
        boolean_setting("plugins.ffmpeg_import","Plugins","Media","FFmpeg Import",true,"Use FFmpeg for broad authoring-time media import.",SettingCapabilityAudio),
        boolean_setting("plugins.reload_on_change","Plugins","Development","Reload Changed Plugins",false,"Rescan compatible plugins during development.",SettingCapabilityNone,true),
    });
    const std::vector<std::string> physicalDependents{"camera.focal_length_mm","camera.sensor_preset","camera.gate_fit","camera.aperture","camera.focus_distance","camera.depth_of_field","camera.show_focus_planes"};
    for(const std::string& id:physicalDependents) (void)result.set_dependencies(id,{{"camera.physical_lens",true,"Enable Use Physical Lens to edit this option."}});
    return result;
}

void EditorSettingsPanelState::open_for(SettingScope newScope, std::string category) {
    open = true;
    scope = newScope;
    if (!category.empty()) selectedCategory = std::move(category);
    searchQuery.clear();
    selectedRow = 0;
    dirty = false;
    valueEditing = false;
    valueEditId.clear();
    valueEditBuffer.clear();
    valueEditReplaceOnNextInput = true;
    status.clear();
    stagedValues.clear();
    stagedClears.clear();
}
void EditorSettingsPanelState::close() noexcept {
    open = false;
    searchQuery.clear();
    selectedRow = 0;
    valueEditing = false;
    valueEditId.clear();
    valueEditBuffer.clear();
    valueEditReplaceOnNextInput = true;
    status.clear();
    stagedValues.clear();
    stagedClears.clear();
    dirty = false;
}
void EditorSettingsPanelState::discard() noexcept {
    valueEditing = false;
    valueEditId.clear();
    valueEditBuffer.clear();
    valueEditReplaceOnNextInput = true;
    status.clear();
    stagedValues.clear();
    stagedClears.clear();
    dirty = false;
}
SettingValue EditorSettingsPanelState::displayed_value(const EditorSettingsRegistry& registry, std::string_view id) const {
    if (const auto it = stagedValues.find(std::string(id)); it != stagedValues.end()) return it->second;
    if (stagedClears.contains(std::string(id))) return registry.inherited_value(id, scope);
    return registry.value(id);
}
bool EditorSettingsPanelState::stage(const EditorSettingsRegistry& registry, std::string_view id,
                                     SettingValue value, std::string* error) {
    EditorSettingsRegistry copy = registry;
    if (!copy.set(scope, id, value, error)) return false;
    stagedClears.erase(std::string(id));
    stagedValues[std::string(id)] = std::move(value);
    dirty = true;
    status = "Staged change";
    return true;
}
bool EditorSettingsPanelState::cycle(const EditorSettingsRegistry& registry, std::string_view id, int direction,
                                     std::string* error) {
    const SettingDefinition* definition = registry.find(id);
    if (!definition) { if (error) *error = "unknown setting"; return false; }
    const SettingAvailability availability = registry.availability(id);
    if (!availability.available) {
        if (error) *error = availability.explanation;
        return false;
    }
    SettingValue current = displayed_value(registry, id);
    if (definition->type == SettingType::Boolean) return stage(registry, id, !std::get<bool>(current), error);
    if (definition->type == SettingType::Enum) {
        const std::string& selected = std::get<std::string>(current);
        auto it = std::find_if(definition->choices.begin(), definition->choices.end(), [&](const SettingChoice& choice) { return choice.value == selected; });
        std::ptrdiff_t index = it == definition->choices.end() ? 0 : std::distance(definition->choices.begin(), it);
        const std::ptrdiff_t count = static_cast<std::ptrdiff_t>(definition->choices.size());
        if (count == 0) { if (error) *error = "setting has no choices"; return false; }
        index = (index + (direction >= 0 ? 1 : -1) + count) % count;
        return stage(registry, id, definition->choices[static_cast<std::size_t>(index)].value, error);
    }
    const double step = definition->step.value_or(1.0) * (direction >= 0 ? 1.0 : -1.0);
    if (definition->type == SettingType::Integer) {
        const auto value = std::get<std::int64_t>(current) + static_cast<std::int64_t>(std::llround(step));
        const auto minimum = static_cast<std::int64_t>(definition->minimum.value_or(static_cast<double>(value)));
        const auto maximum = static_cast<std::int64_t>(definition->maximum.value_or(static_cast<double>(value)));
        return stage(registry, id, std::clamp(value, minimum, maximum), error);
    }
    if (definition->type == SettingType::Float) {
        const double value = std::get<double>(current) + step;
        return stage(registry, id, std::clamp(value, definition->minimum.value_or(value), definition->maximum.value_or(value)), error);
    }
    return begin_value_edit(registry, id, error);
}
bool EditorSettingsPanelState::begin_value_edit(const EditorSettingsRegistry& registry, std::string_view id,
                                                std::string* error) {
    const SettingDefinition* definition = registry.find(id);
    if (!definition) { if (error) *error = "unknown setting"; return false; }
    if (definition->type == SettingType::Boolean || definition->type == SettingType::Enum) {
        if (error) *error = "use left/right to choose this option";
        return false;
    }
    const SettingAvailability availability = registry.availability(id);
    if (!availability.available) {
        if (error) *error = availability.explanation;
        return false;
    }
    valueEditing = true;
    valueEditId = std::string(id);
    valueEditBuffer = setting_value_to_string(displayed_value(registry, id));
    valueEditReplaceOnNextInput = true;
    status = "Type a value, then press Enter";
    return true;
}
void EditorSettingsPanelState::append_value_text(std::string_view text) {
    if (!valueEditing || text.empty()) return;
    if (valueEditReplaceOnNextInput) {
        valueEditBuffer.clear();
        valueEditReplaceOnNextInput = false;
    }
    if (valueEditBuffer.size() + text.size() > 4096U) return;
    valueEditBuffer.append(text);
}
void EditorSettingsPanelState::backspace_value_text() {
    if (!valueEditing || valueEditBuffer.empty()) return;
    valueEditReplaceOnNextInput = false;
    std::size_t erase = valueEditBuffer.size() - 1U;
    while (erase > 0U && (static_cast<unsigned char>(valueEditBuffer[erase]) & 0xC0U) == 0x80U) --erase;
    valueEditBuffer.erase(erase);
}
void EditorSettingsPanelState::cancel_value_edit() noexcept {
    valueEditing = false;
    valueEditId.clear();
    valueEditBuffer.clear();
    status = "Value edit cancelled";
}
bool EditorSettingsPanelState::commit_value_edit(const EditorSettingsRegistry& registry, std::string* error) {
    if (!valueEditing) return false;
    const SettingDefinition* definition = registry.find(valueEditId);
    if (!definition) { if (error) *error = "unknown setting"; return false; }
    SettingValue parsed;
    if (definition->type == SettingType::Integer) {
        std::int64_t value{};
        const char* begin = valueEditBuffer.data();
        const char* end = begin + valueEditBuffer.size();
        const auto result = std::from_chars(begin, end, value);
        if (result.ec != std::errc{} || result.ptr != end) {
            if (error) *error = "enter a whole number";
            return false;
        }
        parsed = value;
    } else if (definition->type == SettingType::Float) {
        double value{};
        const char* begin = valueEditBuffer.data();
        const char* end = begin + valueEditBuffer.size();
        const auto result = std::from_chars(begin, end, value);
        if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(value)) {
            if (error) *error = "enter a finite number";
            return false;
        }
        parsed = value;
    } else if (definition->type == SettingType::String) {
        parsed = valueEditBuffer;
    } else {
        if (error) *error = "this option does not accept text";
        return false;
    }
    if (!stage(registry, valueEditId, std::move(parsed), error)) return false;
    valueEditing = false;
    valueEditId.clear();
    valueEditBuffer.clear();
    status = "Value staged";
    return true;
}
bool EditorSettingsPanelState::reset(const EditorSettingsRegistry& registry, std::string_view id,
                                     std::string* error) {
    if (!registry.find(id)) { if (error) *error = "unknown setting"; return false; }
    stagedValues.erase(std::string(id));
    stagedClears.insert(std::string(id));
    dirty = true;
    valueEditing = false;
    valueEditId.clear();
    valueEditBuffer.clear();
    status = registry.has_override(scope, id) ? "Override will be removed" : "Already inherited";
    return true;
}
std::size_t EditorSettingsPanelState::reset_category(const EditorSettingsRegistry& registry,
                                                     std::string_view categoryName) {
    std::size_t count{};
    for (const SettingDefinition* definition : registry.category(categoryName, true)) {
        stagedValues.erase(definition->id);
        stagedClears.insert(definition->id);
        ++count;
    }
    if (count > 0U) {
        dirty = true;
        status = "Category will inherit its parent/default values";
    }
    return count;
}
bool EditorSettingsPanelState::apply(EditorSettingsRegistry& registry, std::string* error) {
    EditorSettingsRegistry candidate = registry;
    for (const std::string& id : stagedClears) (void)candidate.clear(scope, id);
    for (const auto& [id, value] : stagedValues) if (!candidate.set(scope, id, value, error)) return false;
    registry = std::move(candidate);
    valueEditing = false;
    valueEditId.clear();
    valueEditBuffer.clear();
    stagedValues.clear();
    stagedClears.clear();
    dirty = false;
    status = "Settings applied";
    return true;
}

} // namespace dve::editor
