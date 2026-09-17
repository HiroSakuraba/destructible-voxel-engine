#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dve/component.hpp"

namespace dve::editor {

struct ComponentInspectorPropertyRow {
    std::string name;
    std::string displayName;
    ComponentValue value{};
    bool required{};
    bool assetReference{};
    bool schemaBacked{};
};

struct ComponentInspectorSection {
    ComponentId id{kInvalidComponentId};
    std::string type;
    std::string displayName;
    bool enabled{true};
    bool allowMultiple{true};
    std::vector<ComponentInspectorPropertyRow> properties;
    std::vector<std::string> warnings;
};

[[nodiscard]] std::vector<ComponentInspectorSection> build_component_inspector(
    std::span<const Component> components,
    const ComponentTypeRegistry& registry = ComponentTypeRegistry::make_default());

[[nodiscard]] std::optional<Component> make_component_from_schema(
    std::string_view type,
    ComponentId id,
    const ComponentTypeRegistry& registry = ComponentTypeRegistry::make_default(),
    std::string* error = nullptr);

[[nodiscard]] std::string format_component_value(const ComponentValue& value);
[[nodiscard]] std::optional<ComponentValue> parse_component_value(
    std::string_view text,
    const ComponentValue& typeHint,
    std::string* error = nullptr);

} // namespace dve::editor
