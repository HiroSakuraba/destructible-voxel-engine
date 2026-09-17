#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <utility>

#include "dve/transform.hpp"

namespace dve {

using ComponentId = std::uint64_t;
inline constexpr ComponentId kInvalidComponentId = 0U;

using ComponentValue = std::variant<bool, std::int64_t, double, std::string, Float3, Quaternion>;

struct Component {
    ComponentId id{kInvalidComponentId};
    std::string type;
    bool enabled{true};
    std::map<std::string, ComponentValue, std::less<>> properties;

};

struct ComponentPropertySchema {
    std::string name;
    ComponentValue defaultValue{};
    bool required{};
    bool assetReference{};
    std::string displayName;

    ComponentPropertySchema() = default;
    ComponentPropertySchema(std::string propertyName, ComponentValue defaultPropertyValue = {},
                            bool propertyRequired = false, bool propertyAssetReference = false,
                            std::string propertyDisplayName = {})
        : name(std::move(propertyName)), defaultValue(std::move(defaultPropertyValue)),
          required(propertyRequired), assetReference(propertyAssetReference),
          displayName(std::move(propertyDisplayName)) {}
};

struct ComponentTypeSchema {
    std::string type;
    std::string displayName;
    bool allowMultiple{};
    std::vector<ComponentPropertySchema> properties;
};

class ComponentTypeRegistry {
public:
    [[nodiscard]] bool register_type(ComponentTypeSchema schema, std::string* error = nullptr);
    [[nodiscard]] const ComponentTypeSchema* find(std::string_view type) const noexcept;
    [[nodiscard]] const std::map<std::string, ComponentTypeSchema, std::less<>>& schemas() const noexcept { return schemas_; }
    [[nodiscard]] bool validate(const Component& component, std::string* error = nullptr) const;
    [[nodiscard]] static ComponentTypeRegistry make_default();

private:
    std::map<std::string, ComponentTypeSchema, std::less<>> schemas_;
};

[[nodiscard]] bool component_type_name_is_valid(std::string_view type) noexcept;

[[nodiscard]] bool membership_name_is_valid(std::string_view value) noexcept;
[[nodiscard]] std::vector<std::string> normalize_membership_values(std::span<const std::string> values);
[[nodiscard]] std::vector<std::string> parse_membership_values(std::string_view text);
[[nodiscard]] std::string join_membership_values(std::span<const std::string> values);
[[nodiscard]] bool component_property_name_is_valid(std::string_view name) noexcept;
[[nodiscard]] bool component_value_is_finite(const ComponentValue& value) noexcept;
[[nodiscard]] std::string_view component_value_type_name(const ComponentValue& value) noexcept;
[[nodiscard]] const Component* find_component(std::span<const Component> components, ComponentId id) noexcept;
[[nodiscard]] Component* find_component(std::span<Component> components, ComponentId id) noexcept;
[[nodiscard]] const Component* find_component_by_type(
    std::span<const Component> components, std::string_view type, std::size_t occurrence = 0U) noexcept;
[[nodiscard]] Component* find_component_by_type(
    std::span<Component> components, std::string_view type, std::size_t occurrence = 0U) noexcept;
[[nodiscard]] bool validate_components(
    std::span<const Component> components,
    const ComponentTypeRegistry* registry = nullptr,
    std::string* error = nullptr);

} // namespace dve
