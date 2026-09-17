#include "dve/component.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace dve {
namespace {

bool valid_identifier(std::string_view text, bool allowDot) noexcept {
    if (text.empty() || text.size() > 128U) return false;
    const auto first = static_cast<unsigned char>(text.front());
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_')) return false;
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        const bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                           (allowDot && c == '.');
        if (!valid) return false;
    }
    return true;
}

bool same_value_type(const ComponentValue& a, const ComponentValue& b) noexcept {
    return a.index() == b.index();
}

} // namespace

bool membership_name_is_valid(std::string_view value) noexcept {
    if (value.empty() || value.size() > 64U) return false;
    for (const char raw : value) {
        const auto c = static_cast<unsigned char>(raw);
        const bool valid = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ':';
        if (!valid) return false;
    }
    return true;
}

std::vector<std::string> normalize_membership_values(std::span<const std::string> values) {
    std::vector<std::string> result;
    result.reserve(values.size());
    for (const std::string& value : values) {
        if (!membership_name_is_valid(value)) continue;
        result.push_back(value);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<std::string> parse_membership_values(std::string_view text) {
    std::vector<std::string> values;
    std::string current;
    auto flush = [&]() {
        if (!current.empty()) values.push_back(std::move(current));
        current.clear();
    };
    for (const char c : text) {
        if (c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\n') flush();
        else current.push_back(c);
    }
    flush();
    return normalize_membership_values(values);
}

std::string join_membership_values(std::span<const std::string> values) {
    const auto normalized = normalize_membership_values(values);
    std::string result;
    for (std::size_t i = 0; i < normalized.size(); ++i) {
        if (i) result.push_back(',');
        result += normalized[i];
    }
    return result;
}

bool ComponentTypeRegistry::register_type(ComponentTypeSchema schema, std::string* error) {
    const auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    if (!component_type_name_is_valid(schema.type)) return fail("invalid component type name");
    if (schema.displayName.empty()) schema.displayName = schema.type;
    std::set<std::string, std::less<>> names;
    for (const ComponentPropertySchema& property : schema.properties) {
        if (!component_property_name_is_valid(property.name)) return fail("invalid component property name");
        if (!names.insert(property.name).second) return fail("duplicate component property schema");
        if (!component_value_is_finite(property.defaultValue)) return fail("component property default is not finite");
    }
    if (schemas_.contains(schema.type)) return fail("component type is already registered");
    schemas_.emplace(schema.type, std::move(schema));
    return true;
}

const ComponentTypeSchema* ComponentTypeRegistry::find(std::string_view type) const noexcept {
    const auto it = schemas_.find(type);
    return it == schemas_.end() ? nullptr : &it->second;
}

bool ComponentTypeRegistry::validate(const Component& component, std::string* error) const {
    const auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    if (!component_type_name_is_valid(component.type)) return fail("invalid component type name");
    if (component.id == kInvalidComponentId) return fail("invalid component id");
    for (const auto& [name, value] : component.properties) {
        if (!component_property_name_is_valid(name)) return fail("invalid component property name");
        if (!component_value_is_finite(value)) return fail("component property is not finite");
    }
    const ComponentTypeSchema* schema = find(component.type);
    if (!schema) return true; // Open composition: unregistered components remain valid and serializable.
    for (const ComponentPropertySchema& property : schema->properties) {
        const auto it = component.properties.find(property.name);
        if (it == component.properties.end()) {
            if (property.required) return fail("required component property is missing: " + property.name);
            continue;
        }
        if (!same_value_type(it->second, property.defaultValue))
            return fail("component property type mismatch: " + property.name);
    }
    return true;
}

ComponentTypeRegistry ComponentTypeRegistry::make_default() {
    ComponentTypeRegistry registry;
    std::string ignored;
    (void)registry.register_type({
        "dve.script", "Lua Script", true,
        {{"asset", std::string{}, true, true}, {"startup", false, false, false}}}, &ignored);
    (void)registry.register_type({
        "dve.tags", "Tags", false,
        {{"values", std::string{}, false, false, "Tags"}}}, &ignored);
    (void)registry.register_type({
        "dve.membership", "Tags, Layer, and Groups", false,
        {{"tags", std::string{}, false, false, "Tags"},
         {"groups", std::string{}, false, false, "Groups"},
         {"layer", std::int64_t{0}, false, false, "Layer"}}}, &ignored);
    (void)registry.register_type({
        "dve.lifecycle", "Lifecycle", false,
        {{"spawn", true, false, false, "Spawn"},
         {"enable", true, false, false, "Enable / Disable"},
         {"overlap", true, false, false, "Overlap"},
         {"destroy", true, false, false, "Destroy"}}}, &ignored);
    (void)registry.register_type({
        "dve.pool_member", "Object Pool Member", false,
        {{"pool", std::string{}, true, false, "Pool"},
         {"slot", std::int64_t{0}, true, false, "Slot"}}}, &ignored);
    (void)registry.register_type({
        "dve.spawn", "Spawn Point", true,
        {{"category", std::string{"default"}, false, false}, {"enabled", true, false, false}}}, &ignored);
    (void)registry.register_type({
        "dve.audio_emitter", "Audio Emitter", true,
        {{"asset", std::string{}, true, true}, {"gain", 1.0, false, false}, {"loop", false, false, false}}}, &ignored);
    (void)registry.register_type({
        "dve.skeletal_animator", "Skeletal Animator", false,
        {{"skeleton", std::string{}, true, true, "Skeleton"},
         {"default_clip", std::string{}, false, true, "Default Clip"},
         {"play_on_spawn", true, false, false, "Play on Spawn"},
         {"speed", 1.0, false, false, "Playback Speed"}}}, &ignored);
    (void)registry.register_type({
        "dve.control_rig", "Control Rig", false,
        {{"asset", std::string{}, true, true, "Control Rig"},
         {"enabled", true, false, false, "Enabled"},
         {"evaluate_after_animation", true, false, false, "Post Animation"}}}, &ignored);
    (void)registry.register_type({
        "dve.game_ui", "Gameplay UI", true,
        {{"asset", std::string{}, true, true, "UI Asset"},
         {"locale", std::string{"en"}, false, false, "Locale"},
         {"visible", true, false, false, "Visible"},
         {"accepts_input", true, false, false, "Accepts Input"}}}, &ignored);
    (void)registry.register_type({
        "dve.physics2d_world", "2D Physics World", false,
        {{"backend", std::string{"native_tile"}, false, false, "Backend"},
         {"gravity", Float3{0.0F, 980.0F, 0.0F}, false, false, "Gravity (px/s²)"},
         {"pixels_per_meter", 32.0, false, false, "Pixels per Meter"},
         {"fixed_timestep", 1.0 / 60.0, false, false, "Fixed Time Step"},
         {"substeps", std::int64_t{4}, false, false, "Solver Substeps"},
         {"enable_sleep", true, false, false, "Enable Sleep"},
         {"enable_continuous_collision", true, false, false, "Continuous Collision"}}}, &ignored);
    (void)registry.register_type({
        "dve.physics2d_body", "2D Physics Body", false,
        {{"body_type", std::string{"dynamic"}, false, false, "Body Type"},
         {"linear_velocity", Float3{}, false, false, "Linear Velocity (px/s)"},
         {"angular_velocity", 0.0, false, false, "Angular Velocity"},
         {"linear_damping", 0.0, false, false, "Linear Damping"},
         {"angular_damping", 0.0, false, false, "Angular Damping"},
         {"gravity_scale", 1.0, false, false, "Gravity Scale"},
         {"fixed_rotation", true, false, false, "Fixed Rotation"},
         {"bullet", false, false, false, "Continuous Collision Body"},
         {"allow_sleep", true, false, false, "Allow Sleep"}}}, &ignored);
    (void)registry.register_type({
        "dve.physics2d_collider", "2D Collider", true,
        {{"shape", std::string{"box"}, false, false, "Shape"},
         {"local_center", Float3{}, false, false, "Local Center (px)"},
         {"half_extents", Float3{8.0F, 8.0F, 0.0F}, false, false, "Half Extents (px)"},
         {"radius", 8.0, false, false, "Radius (px)"},
         {"capsule_point_1", Float3{0.0F, -4.0F, 0.0F}, false, false, "Capsule Point 1"},
         {"capsule_point_2", Float3{0.0F, 4.0F, 0.0F}, false, false, "Capsule Point 2"},
         {"convex_vertices", std::string{}, false, false, "Convex Vertices"},
         {"density", 1.0, false, false, "Density"},
         {"friction", 0.4, false, false, "Friction"},
         {"restitution", 0.0, false, false, "Restitution"},
         {"rolling_resistance", 0.0, false, false, "Rolling Resistance"},
         {"tangent_speed", 0.0, false, false, "Conveyor Speed (px/s)"},
         {"sensor", false, false, false, "Sensor"},
         {"one_way_platform", false, false, false, "One-Way Platform"},
         {"category_bits", std::int64_t{1}, false, false, "Collision Category"},
         {"mask_bits", std::int64_t{-1}, false, false, "Collision Mask"},
         {"group_index", std::int64_t{0}, false, false, "Collision Group"}}}, &ignored);
    (void)registry.register_type({
        "dve.physics2d_joint", "2D Physics Joint", true,
        {{"joint_type", std::string{"revolute"}, false, false, "Joint Type"},
         {"body_a", std::string{}, true, false, "Body A"},
         {"body_b", std::string{}, true, false, "Body B"},
         {"local_anchor_a", Float3{}, false, false, "Local Anchor A (px)"},
         {"local_anchor_b", Float3{}, false, false, "Local Anchor B (px)"},
         {"local_axis_a", Float3{1.0F, 0.0F, 0.0F}, false, false, "Local Axis A"},
         {"reference_angle", 0.0, false, false, "Reference Angle"},
         {"enable_limit", false, false, false, "Enable Limit"},
         {"lower_limit", 0.0, false, false, "Lower Limit"},
         {"upper_limit", 0.0, false, false, "Upper Limit"},
         {"enable_motor", false, false, false, "Enable Motor"},
         {"motor_speed", 0.0, false, false, "Motor Speed"},
         {"maximum_motor_force", 0.0, false, false, "Maximum Motor Force"},
         {"maximum_motor_torque", 0.0, false, false, "Maximum Motor Torque"},
         {"enable_spring", false, false, false, "Enable Spring"},
         {"spring_hertz", 0.0, false, false, "Spring Frequency"},
         {"spring_damping_ratio", 0.0, false, false, "Spring Damping Ratio"},
         {"length", 0.0, false, false, "Rest Length (px)"},
         {"minimum_length", 0.0, false, false, "Minimum Length (px)"},
         {"maximum_length", 0.0, false, false, "Maximum Length (px)"},
         {"collide_connected", false, false, false, "Collide Connected"},
         {"break_force", 0.0, false, false, "Break Force"},
         {"break_torque", 0.0, false, false, "Break Torque"}}}, &ignored);
    (void)registry.register_type({
        "dve.sideview_character", "Side-View Character Controller", false,
        {{"maximum_run_speed", 160.0, false, false, "Maximum Run Speed (px/s)"},
         {"ground_acceleration", 1800.0, false, false, "Ground Acceleration"},
         {"ground_deceleration", 2400.0, false, false, "Ground Deceleration"},
         {"air_acceleration", 900.0, false, false, "Air Acceleration"},
         {"air_deceleration", 500.0, false, false, "Air Deceleration"},
         {"jump_speed", 360.0, false, false, "Jump Speed (px/s)"},
         {"coyote_time", 0.10, false, false, "Coyote Time"},
         {"jump_buffer", 0.12, false, false, "Jump Buffer"},
         {"jump_release_factor", 0.48, false, false, "Variable Jump Cut"},
         {"fall_gravity_scale", 1.45, false, false, "Fall Gravity Scale"},
         {"maximum_slope_degrees", 50.0, false, false, "Maximum Slope"},
         {"ground_probe_distance", 4.0, false, false, "Ground Probe Distance"},
         {"wall_probe_distance", 2.0, false, false, "Wall Probe Distance"},
         {"ceiling_probe_distance", 2.0, false, false, "Ceiling Probe Distance"},
         {"ledge_probe_forward", 6.0, false, false, "Ledge Probe Forward"},
         {"ledge_probe_down", 12.0, false, false, "Ledge Probe Down"},
         {"ladder_climb_speed", 110.0, false, false, "Ladder Climb Speed"},
         {"inherit_support_velocity", true, false, false, "Moving Support Inheritance"},
         {"enable_crush_detection", true, false, false, "Crush Detection"},
         {"crush_minimum_closing_speed", 20.0, false, false, "Crush Closing Speed"},
         {"crush_grace_seconds", 0.08, false, false, "Crush Grace"}}}, &ignored);
    (void)registry.register_type({
        "dve.sprite", "Sprite / 2D Visual", true,
        {{"asset", std::string{}, true, true, "Sprite Asset"},
         {"clip", std::string{}, false, false, "Animation Clip"},
         {"visible", true, false, false, "Visible"},
         {"playing", true, false, false, "Playing"},
         {"playback_speed", 1.0, false, false, "Playback Speed"},
         {"sorting_layer", std::int64_t{0}, false, false, "Sorting Layer"},
         {"order_in_layer", std::int64_t{0}, false, false, "Order in Layer"},
         {"sort_depth", 0.0, false, false, "Sort Depth"},
         {"plane", std::string{"XY"}, false, false, "Gameplay Plane"},
         {"pixel_snap", true, false, false, "Pixel Snap"},
         {"flip_x", false, false, false, "Flip X"},
         {"flip_y", false, false, false, "Flip Y"}}}, &ignored);
    (void)registry.register_type({
        "dve.soft_body", "Soft Body / Deformer", false,
        {{"asset", std::string{}, true, true, "Soft Body Asset"},
         {"running", true, false, false, "Running"},
         {"smooth_cloth", false, false, false, "Smooth B-Spline Cloth"},
         {"cloth_columns", std::int64_t{0}, false, false, "Cloth Columns"},
         {"cloth_rows", std::int64_t{0}, false, false, "Cloth Rows"},
         {"embedded_columns", std::int64_t{32}, false, false, "Render Columns"},
         {"embedded_rows", std::int64_t{32}, false, false, "Render Rows"},
         {"solver_iterations", std::int64_t{8}, false, false, "Solver Iterations"},
         {"ground_height", 0.0, false, false, "Ground Height"},
         {"ground_friction", 0.35, false, false, "Ground Friction"}}}, &ignored);
    (void)registry.register_type({
        "dve.prefab_instance", "Prefab Instance", false,
        {{"asset", std::string{}, true, true}, {"instance_id", std::int64_t{0}, true, false}}}, &ignored);
    return registry;
}

bool component_type_name_is_valid(std::string_view type) noexcept { return valid_identifier(type, true); }
bool component_property_name_is_valid(std::string_view name) noexcept { return valid_identifier(name, false); }

bool component_value_is_finite(const ComponentValue& value) noexcept {
    return std::visit([](const auto& item) noexcept {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, double>) return std::isfinite(item);
        else if constexpr (std::is_same_v<T, Float3>)
            return std::isfinite(item.x) && std::isfinite(item.y) && std::isfinite(item.z);
        else if constexpr (std::is_same_v<T, Quaternion>)
            return std::isfinite(item.x) && std::isfinite(item.y) && std::isfinite(item.z) && std::isfinite(item.w) &&
                   (item.x * item.x + item.y * item.y + item.z * item.z + item.w * item.w) > 0.0F;
        else return true;
    }, value);
}

std::string_view component_value_type_name(const ComponentValue& value) noexcept {
    switch (value.index()) {
        case 0: return "bool";
        case 1: return "integer";
        case 2: return "number";
        case 3: return "string";
        case 4: return "float3";
        case 5: return "quaternion";
        default: return "unknown";
    }
}

const Component* find_component(std::span<const Component> components, ComponentId id) noexcept {
    const auto it = std::find_if(components.begin(), components.end(), [id](const Component& component) {
        return component.id == id;
    });
    return it == components.end() ? nullptr : &*it;
}

Component* find_component(std::span<Component> components, ComponentId id) noexcept {
    const auto it = std::find_if(components.begin(), components.end(), [id](const Component& component) {
        return component.id == id;
    });
    return it == components.end() ? nullptr : &*it;
}

const Component* find_component_by_type(
    std::span<const Component> components, std::string_view type, std::size_t occurrence) noexcept {
    for (const Component& component : components) {
        if (component.type == type) {
            if (occurrence == 0U) return &component;
            --occurrence;
        }
    }
    return nullptr;
}

Component* find_component_by_type(
    std::span<Component> components, std::string_view type, std::size_t occurrence) noexcept {
    for (Component& component : components) {
        if (component.type == type) {
            if (occurrence == 0U) return &component;
            --occurrence;
        }
    }
    return nullptr;
}

bool validate_components(
    std::span<const Component> components,
    const ComponentTypeRegistry* registry,
    std::string* error) {
    const auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return false;
    };
    std::set<ComponentId> ids;
    std::map<std::string, std::size_t, std::less<>> typeCounts;
    for (const Component& component : components) {
        if (component.id == kInvalidComponentId || !ids.insert(component.id).second)
            return fail("duplicate or invalid component id");
        if (!component_type_name_is_valid(component.type)) return fail("invalid component type name");
        for (const auto& [name, value] : component.properties) {
            if (!component_property_name_is_valid(name)) return fail("invalid component property name");
            if (!component_value_is_finite(value)) return fail("component property is not finite");
        }
        if (registry) {
            std::string componentError;
            if (!registry->validate(component, &componentError)) return fail(componentError);
            if (const ComponentTypeSchema* schema = registry->find(component.type)) {
                const std::size_t count = ++typeCounts[component.type];
                if (!schema->allowMultiple && count > 1U)
                    return fail("component type does not allow multiple instances: " + component.type);
            }
        }
    }
    return true;
}

} // namespace dve
