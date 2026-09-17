#include "dve/game_ui.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace dve::ui {
namespace {

bool valid_locale(std::string_view value) noexcept {
    if (value.empty() || value.size() > 32U) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_';
    });
}

bool valid_key(std::string_view value) noexcept {
    if (value.empty() || value.size() > 255U) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return character >= 32U && character != 127U;
    });
}

std::string value_string(const UiValue* value) {
    if (!value) return {};
    return std::visit([](const auto& item) {
        std::ostringstream stream;
        stream << item;
        return stream.str();
    }, *value);
}

double numeric_value(const UiValue* value, bool* valid) noexcept {
    *valid = true;
    if (const auto* number = std::get_if<double>(value)) return *number;
    if (const auto* integer = std::get_if<std::int64_t>(value)) return static_cast<double>(*integer);
    *valid = false;
    return 0.0;
}

std::vector<std::string> split(std::string_view value, char delimiter) {
    std::vector<std::string> result;
    std::size_t start{};
    while (start <= value.size()) {
        const std::size_t end = value.find(delimiter, start);
        result.emplace_back(value.substr(start, end == std::string_view::npos ? value.size() - start : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1U;
    }
    return result;
}

std::string expand_choice_blocks(std::string message, const UiDataModel& data) {
    std::size_t cursor{};
    while ((cursor = message.find("{{", cursor)) != std::string::npos) {
        const std::size_t end = message.find("}}", cursor + 2U);
        if (end == std::string::npos) break;
        const auto parts = split(std::string_view(message).substr(cursor + 2U, end - cursor - 2U), '|');
        if (parts.size() < 3U) { cursor = end + 2U; continue; }
        const std::size_t colon = parts.front().find(':');
        if (colon == std::string::npos) { cursor = end + 2U; continue; }
        const std::string kind = parts.front().substr(0U, colon);
        const std::string argument = parts.front().substr(colon + 1U);
        std::string selector;
        if (kind == "plural") {
            bool valid{};
            const double number = numeric_value(data.get(argument), &valid);
            selector = valid && std::abs(number - 1.0) < 1.0e-9 ? "one" : "other";
        } else if (kind == "select") selector = value_string(data.get(argument));
        else { cursor = end + 2U; continue; }
        std::string replacement;
        for (std::size_t index = 1U; index < parts.size(); ++index) {
            const std::size_t equals = parts[index].find('=');
            if (equals == std::string::npos) continue;
            const std::string option = parts[index].substr(0U, equals);
            if (option == selector || (replacement.empty() && option == "other"))
                replacement = parts[index].substr(equals + 1U);
            if (option == selector) break;
        }
        message.replace(cursor, end + 2U - cursor, replacement);
        cursor += replacement.size();
    }
    return message;
}

std::string expand_placeholders(std::string message, const UiDataModel& data) {
    std::size_t cursor{};
    while ((cursor = message.find('{', cursor)) != std::string::npos) {
        const std::size_t end = message.find('}', cursor + 1U);
        if (end == std::string::npos) break;
        const std::string key = message.substr(cursor + 1U, end - cursor - 1U);
        const UiValue* value = data.get(key);
        if (!value) { cursor = end + 1U; continue; }
        const std::string replacement = value_string(value);
        message.replace(cursor, end + 1U - cursor, replacement);
        cursor += replacement.size();
    }
    return message;
}

std::string pseudo_localize(std::string_view value) {
    std::string result{"[!! "};
    result.reserve(value.size() * 2U + 8U);
    for (char character : value) {
        switch (character) {
            case 'a': result += "á"; break; case 'A': result += "Á"; break;
            case 'e': result += "ë"; break; case 'E': result += "Ë"; break;
            case 'i': result += "ï"; break; case 'I': result += "Ï"; break;
            case 'o': result += "ô"; break; case 'O': result += "Ô"; break;
            case 'u': result += "ü"; break; case 'U': result += "Ü"; break;
            default: result.push_back(character); break;
        }
    }
    result += " !!]";
    return result;
}

} // namespace

bool UiLocalizationTable::validate(std::string* error) const {
    auto fail = [&](std::string message) { if (error) *error = std::move(message); return false; };
    if (!valid_locale(locale) || (!fallbackLocale.empty() && !valid_locale(fallbackLocale)) ||
        fallbackLocale == locale || messages.size() > 100000U)
        return fail("UI localization locale or fallback is invalid");
    for (const auto& [key, message] : messages)
        if (!valid_key(key) || message.size() > 65535U)
            return fail("UI localization key or message is invalid");
    return true;
}

bool UiLocalizer::register_table(UiLocalizationTable table, std::string* error) {
    if (!table.validate(error)) return false;
    tables_.insert_or_assign(table.locale, std::move(table));
    return true;
}

bool UiLocalizer::set_locale(std::string locale, std::string* error) {
    if (!valid_locale(locale) || !tables_.contains(locale)) {
        if (error) *error = "UI locale is invalid or not registered";
        return false;
    }
    locale_ = std::move(locale);
    return true;
}

std::string UiLocalizer::resolve(std::string_view key, const UiDataModel& data) const {
    std::string message;
    std::string current = locale_;
    std::vector<std::string> visited;
    for (std::size_t depth = 0U; depth < 16U && !current.empty(); ++depth) {
        if (std::find(visited.begin(), visited.end(), current) != visited.end()) break;
        visited.push_back(current);
        const auto table = tables_.find(current);
        if (table == tables_.end()) break;
        if (const auto found = table->second.messages.find(key); found != table->second.messages.end()) {
            message = found->second;
            break;
        }
        current = table->second.fallbackLocale;
    }
    if (message.empty()) {
        const std::string missing(key);
        if (std::find(missingKeys_.begin(), missingKeys_.end(), missing) == missingKeys_.end())
            missingKeys_.push_back(missing);
        message = missing;
    }
    message = expand_placeholders(expand_choice_blocks(std::move(message), data), data);
    return pseudoLocalization_ ? pseudo_localize(message) : message;
}

} // namespace dve::ui
