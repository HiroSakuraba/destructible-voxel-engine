#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace dve::ai {

class JsonValue {
public:
    using Object = std::map<std::string, JsonValue, std::less<>>;
    using Array = std::vector<JsonValue>;
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    JsonValue() : value_(nullptr) {}
    JsonValue(std::nullptr_t) : value_(nullptr) {}
    JsonValue(bool value) : value_(value) {}
    JsonValue(int value) : value_(static_cast<double>(value)) {}
    JsonValue(std::uint64_t value) : value_(static_cast<double>(value)) {}
    JsonValue(double value) : value_(value) {}
    JsonValue(std::string value) : value_(std::move(value)) {}
    JsonValue(const char* value) : value_(std::string(value ? value : "")) {}
    JsonValue(Array value) : value_(std::move(value)) {}
    JsonValue(Object value) : value_(std::move(value)) {}

    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] bool is_bool() const noexcept;
    [[nodiscard]] bool is_number() const noexcept;
    [[nodiscard]] bool is_string() const noexcept;
    [[nodiscard]] bool is_array() const noexcept;
    [[nodiscard]] bool is_object() const noexcept;

    [[nodiscard]] bool as_bool(bool fallback = false) const noexcept;
    [[nodiscard]] double as_number(double fallback = 0.0) const noexcept;
    [[nodiscard]] std::string_view as_string(std::string_view fallback = {}) const noexcept;
    [[nodiscard]] const Array& as_array() const;
    [[nodiscard]] Array& as_array();
    [[nodiscard]] const Object& as_object() const;
    [[nodiscard]] Object& as_object();

    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;
    [[nodiscard]] JsonValue* find(std::string_view key) noexcept;
    JsonValue& operator[](std::string key);

    [[nodiscard]] const Storage& storage() const noexcept { return value_; }
private:
    Storage value_;
};

struct JsonParseResult {
    std::optional<JsonValue> value;
    std::string error;
    std::size_t errorOffset{};
};

[[nodiscard]] JsonParseResult parse_json(std::string_view text);
[[nodiscard]] std::string stringify_json(const JsonValue& value, bool pretty = false);
[[nodiscard]] std::string json_escape(std::string_view text);

} // namespace dve::ai
