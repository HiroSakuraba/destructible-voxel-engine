#include "dve/editor_component_inspector.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <utility>

namespace dve::editor {
namespace {

std::string title_from_identifier(std::string_view name) {
    std::string result;
    result.reserve(name.size());
    bool capitalize = true;
    for (const char c : name) {
        if (c == '_' || c == '-') {
            result.push_back(' ');
            capitalize = true;
            continue;
        }
        result.push_back(capitalize && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
        capitalize = false;
    }
    return result;
}

std::vector<double> parse_numbers(std::string_view text) {
    std::string normalized(text);
    for (char& c : normalized) if (c == ',' || c == ';') c = ' ';
    std::istringstream input(normalized);
    std::vector<double> values;
    double value{};
    while (input >> value) values.push_back(value);
    if (!input.eof()) return {};
    return values;
}

} // namespace

std::vector<ComponentInspectorSection> build_component_inspector(
    std::span<const Component> components,
    const ComponentTypeRegistry& registry) {
    std::vector<ComponentInspectorSection> result;
    result.reserve(components.size());
    for (const Component& component : components) {
        ComponentInspectorSection section;
        section.id = component.id;
        section.type = component.type;
        section.enabled = component.enabled;
        const ComponentTypeSchema* schema = registry.find(component.type);
        section.displayName = schema ? schema->displayName : component.type;
        section.allowMultiple = !schema || schema->allowMultiple;
        if (schema) {
            for (const ComponentPropertySchema& property : schema->properties) {
                const auto it = component.properties.find(property.name);
                ComponentInspectorPropertyRow row;
                row.name = property.name;
                row.displayName = property.displayName.empty()
                    ? title_from_identifier(property.name) : property.displayName;
                row.value = it == component.properties.end() ? property.defaultValue : it->second;
                row.required = property.required;
                row.assetReference = property.assetReference;
                row.schemaBacked = true;
                section.properties.push_back(std::move(row));
                if (property.required && it == component.properties.end())
                    section.warnings.push_back("Missing required property: " + property.name);
            }
        }
        for (const auto& [name, value] : component.properties) {
            if (schema && std::any_of(schema->properties.begin(), schema->properties.end(),
                                      [&](const ComponentPropertySchema& property) { return property.name == name; }))
                continue;
            section.properties.push_back({name, title_from_identifier(name), value, false, false, false});
        }
        std::sort(section.properties.begin(), section.properties.end(), [](const auto& a, const auto& b) {
            if (a.schemaBacked != b.schemaBacked) return a.schemaBacked > b.schemaBacked;
            return a.name < b.name;
        });
        result.push_back(std::move(section));
    }
    return result;
}

std::optional<Component> make_component_from_schema(
    std::string_view type,
    ComponentId id,
    const ComponentTypeRegistry& registry,
    std::string* error) {
    const ComponentTypeSchema* schema = registry.find(type);
    if (!schema) {
        if (error) *error = "component type is not registered";
        return std::nullopt;
    }
    Component component;
    component.id = id;
    component.type = schema->type;
    for (const ComponentPropertySchema& property : schema->properties)
        component.properties.emplace(property.name, property.defaultValue);
    if (!registry.validate(component, error)) return std::nullopt;
    return component;
}

std::string format_component_value(const ComponentValue& value) {
    std::ostringstream output;
    output << std::setprecision(9);
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, bool>) output << (item ? "true" : "false");
        else if constexpr (std::is_same_v<T, Float3>) output << item.x << ' ' << item.y << ' ' << item.z;
        else if constexpr (std::is_same_v<T, Quaternion>) output << item.x << ' ' << item.y << ' ' << item.z << ' ' << item.w;
        else output << item;
    }, value);
    return output.str();
}

std::optional<ComponentValue> parse_component_value(
    std::string_view text,
    const ComponentValue& typeHint,
    std::string* error) {
    const auto fail = [&](std::string message) -> std::optional<ComponentValue> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    switch (typeHint.index()) {
        case 0: {
            if (text == "true" || text == "1" || text == "yes" || text == "on") return ComponentValue{true};
            if (text == "false" || text == "0" || text == "no" || text == "off") return ComponentValue{false};
            return fail("expected a boolean value");
        }
        case 1: {
            std::int64_t value{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return fail("expected an integer");
            return ComponentValue{value};
        }
        case 2: {
            std::string storage(text);
            char* end = nullptr;
            const double value = std::strtod(storage.c_str(), &end);
            if (!end || end != storage.c_str() + storage.size() || !std::isfinite(value)) return fail("expected a finite number");
            return ComponentValue{value};
        }
        case 3: return ComponentValue{std::string(text)};
        case 4: {
            const auto values = parse_numbers(text);
            if (values.size() != 3U) return fail("expected three numbers");
            return ComponentValue{Float3{static_cast<float>(values[0]), static_cast<float>(values[1]), static_cast<float>(values[2])}};
        }
        case 5: {
            const auto values = parse_numbers(text);
            if (values.size() != 4U) return fail("expected four numbers");
            Quaternion q{static_cast<float>(values[0]), static_cast<float>(values[1]),
                         static_cast<float>(values[2]), static_cast<float>(values[3])};
            if (!component_value_is_finite(ComponentValue{q})) return fail("expected a non-zero finite quaternion");
            return ComponentValue{normalize(q)};
        }
        default: return fail("unsupported component value type");
    }
}

} // namespace dve::editor
