#include "dve/editor_number_field.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dve::editor {
namespace {

constexpr double kPi = 3.14159265358979323846;

class Parser {
public:
    Parser(std::string_view text, NumberFieldUnit unit) : text_(text), unit_(unit) {}

    std::optional<double> parse(std::string& error) {
        const std::optional<double> value = expression();
        skip_space();
        if (!value) { error = error_.empty() ? "Not a number" : error_; return std::nullopt; }
        if (position_ != text_.size()) {
            error = "Unexpected \"" + std::string(text_.substr(position_)) + "\"";
            return std::nullopt;
        }
        if (!std::isfinite(*value)) { error = "The result is not a finite number"; return std::nullopt; }
        return value;
    }

private:
    std::string_view text_;
    NumberFieldUnit unit_;
    std::size_t position_{};
    std::string error_;
    int depth_{};

    void skip_space() {
        while (position_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[position_]))) ++position_;
    }
    bool take(char c) {
        skip_space();
        if (position_ < text_.size() && text_[position_] == c) { ++position_; return true; }
        return false;
    }
    std::optional<double> fail(std::string message) {
        if (error_.empty()) error_ = std::move(message);
        return std::nullopt;
    }

    std::optional<double> expression() {
        std::optional<double> value = term();
        while (value) {
            if (take('+')) { const auto rhs = term(); if (!rhs) return std::nullopt; *value += *rhs; }
            else if (take('-')) { const auto rhs = term(); if (!rhs) return std::nullopt; *value -= *rhs; }
            else break;
        }
        return value;
    }
    std::optional<double> term() {
        std::optional<double> value = unary();
        while (value) {
            if (take('*')) { const auto rhs = unary(); if (!rhs) return std::nullopt; *value *= *rhs; }
            else if (take('/')) {
                const auto rhs = unary();
                if (!rhs) return std::nullopt;
                if (*rhs == 0.0) return fail("Division by zero");
                *value /= *rhs;
            } else break;
        }
        return value;
    }
    std::optional<double> unary() {
        if (++depth_ > 64) return fail("Too deeply nested");
        std::optional<double> result;
        if (take('-')) { result = unary(); if (result) *result = -*result; }
        else if (take('+')) result = unary();
        else result = primary();
        --depth_;
        return result;
    }
    std::optional<double> primary() {
        skip_space();
        double value = 0.0;
        if (take('(')) {
            const auto inner = expression();
            if (!inner) return std::nullopt;
            if (!take(')')) return fail("Missing \")\"");
            value = *inner;
        } else if (text_.substr(position_, 2) == "pi") {
            position_ += 2;
            value = kPi;
        } else {
            const std::size_t start = position_;
            while (position_ < text_.size() &&
                   (std::isdigit(static_cast<unsigned char>(text_[position_])) || text_[position_] == '.'))
                ++position_;
            if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E') && position_ > start &&
                position_ + 1 < text_.size() &&
                (std::isdigit(static_cast<unsigned char>(text_[position_ + 1])) || text_[position_ + 1] == '-' || text_[position_ + 1] == '+')) {
                position_ += 2;
                while (position_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[position_]))) ++position_;
            }
            if (position_ == start) return fail(position_ < text_.size() ? "Unexpected \"" + std::string(1, text_[position_]) + "\""
                                                                         : "Missing a number");
            const std::string digits(text_.substr(start, position_ - start));
            char* end = nullptr;
            value = std::strtod(digits.c_str(), &end);
            if (!end || *end != '\0') return fail("Not a number: " + digits);
        }
        return with_unit(value);
    }
    // An optional unit after a number or parenthesis; feet may be followed by inches (1'6").
    std::optional<double> with_unit(double value) {
        skip_space();
        const std::size_t start = position_;
        if (position_ < text_.size() && (text_[position_] == '\'' || text_[position_] == '"')) {
            if (unit_ != NumberFieldUnit::Meters) return fail("Lengths are not allowed here");
            const bool feet = text_[position_] == '\'';
            ++position_;
            double meters = value * (feet ? 0.3048 : 0.0254);
            if (feet) {
                // 1'6" : inches follow the feet directly.
                const std::size_t save = position_;
                skip_space();
                if (position_ < text_.size() && (std::isdigit(static_cast<unsigned char>(text_[position_])) || text_[position_] == '.')) {
                    const std::size_t numberStart = position_;
                    while (position_ < text_.size() &&
                           (std::isdigit(static_cast<unsigned char>(text_[position_])) || text_[position_] == '.'))
                        ++position_;
                    if (position_ < text_.size() && text_[position_] == '"') {
                        meters += std::strtod(std::string(text_.substr(numberStart, position_ - numberStart)).c_str(), nullptr) * 0.0254;
                        ++position_;
                    } else position_ = save;
                } else position_ = save;
            }
            return meters;
        }
        if (text_.substr(position_, 2) == "\xC2\xB0") {
            if (unit_ != NumberFieldUnit::Degrees) return fail("Angles are not allowed here");
            position_ += 2;
            return value;
        }
        while (position_ < text_.size() && std::isalpha(static_cast<unsigned char>(text_[position_]))) ++position_;
        if (position_ == start) return value;
        std::string word(text_.substr(start, position_ - start));
        for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (unit_ == NumberFieldUnit::Meters) {
            if (word == "m") return value;
            if (word == "cm") return value * 0.01;
            if (word == "mm") return value * 0.001;
            if (word == "km") return value * 1000.0;
            if (word == "in") return value * 0.0254;
            if (word == "ft") return value * 0.3048;
            if (word == "deg" || word == "rad" || word == "turn") return fail("Angles are not allowed here");
        } else if (unit_ == NumberFieldUnit::Degrees) {
            if (word == "deg") return value;
            if (word == "rad") return value * 180.0 / kPi;
            if (word == "turn" || word == "turns") return value * 360.0;
            if (word == "m" || word == "cm" || word == "mm" || word == "ft" || word == "in") return fail("Lengths are not allowed here");
        } else {
            return fail("Units are not allowed here: \"" + word + "\"");
        }
        return fail("Unknown unit \"" + word + "\"");
    }
};

}  // namespace

NumberFieldParse parse_number_field(std::string_view text, NumberFieldUnit unit, double current) {
    NumberFieldParse result;
    std::size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first]))) ++first;
    text.remove_prefix(first);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    if (text.empty()) { result.error = "Type a value"; return result; }
    char op = 0;
    if (text.size() >= 2 && text[1] == '=' && (text[0] == '+' || text[0] == '-' || text[0] == '*' || text[0] == '/')) {
        op = text[0];
        text.remove_prefix(2);
    }
    // A relative factor or divisor is a plain number, not a length or angle.
    Parser parser(text, op == '*' || op == '/' ? NumberFieldUnit::Count : unit);
    const std::optional<double> value = parser.parse(result.error);
    if (!value) return result;
    switch (op) {
    case '+': result.value = current + *value; break;
    case '-': result.value = current - *value; break;
    case '*': result.value = current * *value; break;
    case '/':
        if (*value == 0.0) { result.error = "Division by zero"; return result; }
        result.value = current / *value;
        break;
    default: result.value = *value; break;
    }
    if (!std::isfinite(result.value)) { result.error = "The result is not a finite number"; return result; }
    result.ok = true;
    return result;
}

std::string format_number_field(double value, NumberFieldUnit unit) {
    const int decimals = unit == NumberFieldUnit::Meters ? 4 : unit == NumberFieldUnit::Degrees ? 2 : 0;
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.*f", decimals, value);
    std::string text(buffer);
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') text.pop_back();
        if (!text.empty() && text.back() == '.') text.pop_back();
    }
    if (text == "-0") text = "0";
    return text;
}

}  // namespace dve::editor
