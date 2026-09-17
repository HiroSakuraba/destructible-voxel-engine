#include "dve/asset_cooker.hpp"
#include "dve/dvox.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <queue>
#include <setjmp.h>
#include <set>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>

#include <jpeglib.h>
#include <png.h>

namespace dve {

namespace {

constexpr float kDegenerateAreaSquared = 1.0e-18F;
constexpr float kAxisEpsilonSquared = 1.0e-20F;

struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue, std::less<>>;
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;
    Storage storage{};

    [[nodiscard]] bool is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(storage); }
    [[nodiscard]] bool is_bool() const noexcept { return std::holds_alternative<bool>(storage); }
    [[nodiscard]] bool is_number() const noexcept { return std::holds_alternative<double>(storage); }
    [[nodiscard]] bool is_string() const noexcept { return std::holds_alternative<std::string>(storage); }
    [[nodiscard]] bool is_array() const noexcept { return std::holds_alternative<Array>(storage); }
    [[nodiscard]] bool is_object() const noexcept { return std::holds_alternative<Object>(storage); }

    [[nodiscard]] const Array& array() const { return std::get<Array>(storage); }
    [[nodiscard]] const Object& object() const { return std::get<Object>(storage); }
    [[nodiscard]] const std::string& string() const { return std::get<std::string>(storage); }
    [[nodiscard]] double number() const { return std::get<double>(storage); }
    [[nodiscard]] bool boolean() const { return std::get<bool>(storage); }

    [[nodiscard]] const JsonValue* find(std::string_view key) const {
        if (!is_object()) return nullptr;
        const auto it = object().find(key);
        return it == object().end() ? nullptr : &it->second;
    }
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    JsonValue parse() {
        skip_whitespace();
        JsonValue value = parse_value();
        skip_whitespace();
        if (position_ != text_.size()) fail("trailing characters after JSON value");
        return value;
    }

private:
    std::string_view text_;
    std::size_t position_{};

    [[noreturn]] void fail(std::string_view message) const {
        throw std::runtime_error(
            "JSON parse error at byte " + std::to_string(position_) + ": " + std::string(message));
    }

    void skip_whitespace() {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++position_;
        }
    }

    char consume() {
        if (position_ >= text_.size()) fail("unexpected end of input");
        return text_[position_++];
    }

    bool consume_if(char expected) {
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect_literal(std::string_view literal) {
        if (text_.substr(position_, literal.size()) != literal) fail("invalid literal");
        position_ += literal.size();
    }

    JsonValue parse_value() {
        skip_whitespace();
        if (position_ >= text_.size()) fail("expected value");
        switch (text_[position_]) {
        case 'n': expect_literal("null"); return JsonValue{nullptr};
        case 't': expect_literal("true"); return JsonValue{true};
        case 'f': expect_literal("false"); return JsonValue{false};
        case '"': return JsonValue{parse_string()};
        case '[': return JsonValue{parse_array()};
        case '{': return JsonValue{parse_object()};
        default: return JsonValue{parse_number()};
        }
    }

    static void append_utf8(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7FU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else if (codepoint <= 0xFFFFU) {
            output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        } else {
            output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
        }
    }

    std::uint32_t parse_hex4() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = consume();
            value <<= 4U;
            if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else fail("invalid unicode escape");
        }
        return value;
    }

    std::string parse_string() {
        if (consume() != '"') fail("expected string");
        std::string output;
        while (position_ < text_.size()) {
            const char c = consume();
            if (c == '"') return output;
            if (static_cast<unsigned char>(c) < 0x20U) fail("control character in string");
            if (c != '\\') {
                output.push_back(c);
                continue;
            }
            const char escaped = consume();
            switch (escaped) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                std::uint32_t codepoint = parse_hex4();
                if (codepoint >= 0xD800U && codepoint <= 0xDBFFU) {
                    if (consume() != '\\' || consume() != 'u') fail("expected low surrogate");
                    const std::uint32_t low = parse_hex4();
                    if (low < 0xDC00U || low > 0xDFFFU) fail("invalid low surrogate");
                    codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) + (low - 0xDC00U);
                }
                append_utf8(output, codepoint);
                break;
            }
            default: fail("invalid string escape");
            }
        }
        fail("unterminated string");
    }

    double parse_number() {
        const std::size_t start = position_;
        if (consume_if('-')) {}
        if (position_ >= text_.size()) fail("invalid number");
        if (consume_if('0')) {
            // A leading zero must stand alone before a fraction or exponent.
        } else {
            if (text_[position_] < '1' || text_[position_] > '9') fail("invalid number");
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
        }
        if (consume_if('.')) {
            const std::size_t fractionStart = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
            if (fractionStart == position_) fail("invalid fraction");
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
            const std::size_t exponentStart = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
            if (exponentStart == position_) fail("invalid exponent");
        }
        const std::string token(text_.substr(start, position_ - start));
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end == nullptr || static_cast<std::size_t>(end - token.c_str()) != token.size() || !std::isfinite(value)) {
            fail("number is not finite");
        }
        return value;
    }

    JsonValue::Array parse_array() {
        if (consume() != '[') fail("expected array");
        JsonValue::Array output;
        skip_whitespace();
        if (consume_if(']')) return output;
        for (;;) {
            output.push_back(parse_value());
            skip_whitespace();
            if (consume_if(']')) return output;
            if (!consume_if(',')) fail("expected comma in array");
            skip_whitespace();
        }
    }

    JsonValue::Object parse_object() {
        if (consume() != '{') fail("expected object");
        JsonValue::Object output;
        skip_whitespace();
        if (consume_if('}')) return output;
        for (;;) {
            if (position_ >= text_.size() || text_[position_] != '"') fail("expected object key");
            std::string key = parse_string();
            skip_whitespace();
            if (!consume_if(':')) fail("expected colon after object key");
            skip_whitespace();
            const auto [it, inserted] = output.emplace(std::move(key), parse_value());
            (void)it;
            if (!inserted) fail("duplicate object key");
            skip_whitespace();
            if (consume_if('}')) return output;
            if (!consume_if(',')) fail("expected comma in object");
            skip_whitespace();
        }
    }
};

void append_json_escaped(std::string& output, std::string_view text) {
    output.push_back('"');
    for (const unsigned char c : text) {
        switch (c) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (c < 0x20U) {
                static constexpr char digits[] = "0123456789abcdef";
                output += "\\u00";
                output.push_back(digits[c >> 4U]);
                output.push_back(digits[c & 0x0FU]);
            } else {
                output.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    output.push_back('"');
}

void append_canonical_json(std::string& output, const JsonValue& value) {
    if (value.is_null()) {
        output += "null";
    } else if (value.is_bool()) {
        output += value.boolean() ? "true" : "false";
    } else if (value.is_number()) {
        std::ostringstream stream;
        stream << std::setprecision(17) << value.number();
        output += stream.str();
    } else if (value.is_string()) {
        append_json_escaped(output, value.string());
    } else if (value.is_array()) {
        output.push_back('[');
        bool first = true;
        for (const JsonValue& element : value.array()) {
            if (!first) output.push_back(',');
            first = false;
            append_canonical_json(output, element);
        }
        output.push_back(']');
    } else {
        output.push_back('{');
        bool first = true;
        for (const auto& [key, element] : value.object()) {
            if (!first) output.push_back(',');
            first = false;
            append_json_escaped(output, key);
            output.push_back(':');
            append_canonical_json(output, element);
        }
        output.push_back('}');
    }
}

[[nodiscard]] std::string canonical_json(const JsonValue& value) {
    std::string output;
    append_canonical_json(output, value);
    return output;
}

[[nodiscard]] std::vector<std::byte> read_binary_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("unable to open file: " + path.string());
    const std::streamoff length = input.tellg();
    if (length < 0) throw std::runtime_error("unable to determine file size: " + path.string());
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> data(static_cast<std::size_t>(length));
    if (!data.empty() && !input.read(reinterpret_cast<char*>(data.data()), length)) {
        throw std::runtime_error("unable to read file: " + path.string());
    }
    return data;
}

[[nodiscard]] std::string read_text_file(const std::filesystem::path& path) {
    const std::vector<std::byte> bytes = read_binary_file(path);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

[[nodiscard]] std::uint32_t read_u32_le(const std::byte* data) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[0])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[1])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[2])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[3])) << 24U);
}

[[nodiscard]] std::vector<std::byte> decode_base64(std::string_view encoded) {
    static constexpr std::array<std::int16_t, 256> table = [] {
        std::array<std::int16_t, 256> values{};
        values.fill(-1);
        for (int i = 0; i < 26; ++i) {
            values[static_cast<std::size_t>('A' + i)] = static_cast<std::int16_t>(i);
            values[static_cast<std::size_t>('a' + i)] = static_cast<std::int16_t>(26 + i);
        }
        for (int i = 0; i < 10; ++i) values[static_cast<std::size_t>('0' + i)] = static_cast<std::int16_t>(52 + i);
        values[static_cast<std::size_t>('+')] = 62;
        values[static_cast<std::size_t>('/')] = 63;
        return values;
    }();

    std::vector<std::byte> output;
    output.reserve((encoded.size() * 3U) / 4U);
    std::uint32_t accumulator = 0;
    int bits = 0;
    for (char c : encoded) {
        if (c == '=') break;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        const std::int16_t value = table[static_cast<unsigned char>(c)];
        if (value < 0) throw std::runtime_error("invalid base64 data");
        accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<std::byte>((accumulator >> static_cast<unsigned>(bits)) & 0xFFU));
        }
    }
    return output;
}

[[nodiscard]] std::uint32_t json_u32(const JsonValue* value, std::uint32_t fallback = 0) {
    if (value == nullptr) return fallback;
    if (!value->is_number()) throw std::runtime_error("expected unsigned integer JSON value");
    const double number = value->number();
    if (number < 0.0 || number > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
        std::floor(number) != number) {
        throw std::runtime_error("JSON integer is outside uint32 range");
    }
    return static_cast<std::uint32_t>(number);
}

[[nodiscard]] std::size_t json_size(const JsonValue* value, std::size_t fallback = 0) {
    const std::uint32_t asU32 = json_u32(value, static_cast<std::uint32_t>(std::min<std::size_t>(fallback, std::numeric_limits<std::uint32_t>::max())));
    return asU32;
}

[[nodiscard]] std::uint64_t json_u64(const JsonValue* value, std::uint64_t fallback = 0) {
    if (value == nullptr) return fallback;
    if (!value->is_number()) throw std::runtime_error("expected unsigned integer JSON value");
    const double number = value->number();
    constexpr double kMaximumExactJsonInteger = 9007199254740991.0; // 2^53 - 1
    if (number < 0.0 || number > kMaximumExactJsonInteger || std::floor(number) != number) {
        throw std::runtime_error("JSON integer is outside the exact uint64 sidecar range");
    }
    return static_cast<std::uint64_t>(number);
}

[[nodiscard]] float json_float(const JsonValue* value, float fallback = 0.0F) {
    if (value == nullptr) return fallback;
    if (!value->is_number()) throw std::runtime_error("expected numeric JSON value");
    const double number = value->number();
    if (!std::isfinite(number) || number < -static_cast<double>(std::numeric_limits<float>::max()) ||
        number > static_cast<double>(std::numeric_limits<float>::max())) {
        throw std::runtime_error("JSON number is outside float range");
    }
    return static_cast<float>(number);
}

[[nodiscard]] bool json_bool(const JsonValue* value, bool fallback = false) {
    if (value == nullptr) return fallback;
    if (!value->is_bool()) throw std::runtime_error("expected boolean JSON value");
    return value->boolean();
}

[[nodiscard]] std::string json_string(const JsonValue* value, std::string fallback = {}) {
    if (value == nullptr) return fallback;
    if (!value->is_string()) throw std::runtime_error("expected string JSON value");
    return value->string();
}

[[nodiscard]] std::vector<float> json_float_array(const JsonValue* value, std::size_t expected) {
    if (value == nullptr) return {};
    if (!value->is_array() || value->array().size() != expected) throw std::runtime_error("unexpected JSON array size");
    std::vector<float> result;
    result.reserve(expected);
    for (const JsonValue& element : value->array()) result.push_back(json_float(&element));
    return result;
}

struct GltfBufferView {
    std::uint32_t buffer{};
    std::size_t offset{};
    std::size_t length{};
    std::size_t stride{};
};

struct GltfAccessor {
    std::optional<std::uint32_t> bufferView;
    std::size_t offset{};
    std::uint32_t componentType{};
    std::size_t count{};
    std::uint32_t components{};
    bool normalized{};
};

[[nodiscard]] std::uint32_t component_count(std::string_view type) {
    if (type == "SCALAR") return 1;
    if (type == "VEC2") return 2;
    if (type == "VEC3") return 3;
    if (type == "VEC4") return 4;
    if (type == "MAT2") return 4;
    if (type == "MAT3") return 9;
    if (type == "MAT4") return 16;
    throw std::runtime_error("unsupported glTF accessor type: " + std::string(type));
}

[[nodiscard]] std::size_t component_size(std::uint32_t componentType) {
    switch (componentType) {
    case 5120: case 5121: return 1;
    case 5122: case 5123: return 2;
    case 5125: case 5126: return 4;
    default: throw std::runtime_error("unsupported glTF componentType");
    }
}

[[nodiscard]] double read_component(const std::byte* pointer, std::uint32_t componentType, bool normalized) {
    switch (componentType) {
    case 5120: {
        std::int8_t value{};
        std::memcpy(&value, pointer, sizeof(value));
        return normalized ? std::max(-1.0, static_cast<double>(value) / 127.0) : static_cast<double>(value);
    }
    case 5121: {
        std::uint8_t value{};
        std::memcpy(&value, pointer, sizeof(value));
        return normalized ? static_cast<double>(value) / 255.0 : static_cast<double>(value);
    }
    case 5122: {
        std::int16_t value{};
        std::memcpy(&value, pointer, sizeof(value));
        if constexpr (std::endian::native == std::endian::big) value = static_cast<std::int16_t>(std::byteswap(static_cast<std::uint16_t>(value)));
        return normalized ? std::max(-1.0, static_cast<double>(value) / 32767.0) : static_cast<double>(value);
    }
    case 5123: {
        std::uint16_t value{};
        std::memcpy(&value, pointer, sizeof(value));
        if constexpr (std::endian::native == std::endian::big) value = std::byteswap(value);
        return normalized ? static_cast<double>(value) / 65535.0 : static_cast<double>(value);
    }
    case 5125: {
        std::uint32_t value{};
        std::memcpy(&value, pointer, sizeof(value));
        if constexpr (std::endian::native == std::endian::big) value = std::byteswap(value);
        return normalized ? static_cast<double>(value) / 4294967295.0 : static_cast<double>(value);
    }
    case 5126: {
        std::uint32_t bits{};
        std::memcpy(&bits, pointer, sizeof(bits));
        if constexpr (std::endian::native == std::endian::big) bits = std::byteswap(bits);
        const float value = std::bit_cast<float>(bits);
        if (!std::isfinite(value)) throw std::runtime_error("non-finite float in glTF accessor");
        return static_cast<double>(value);
    }
    default: throw std::runtime_error("unsupported component type");
    }
}

struct GltfDocument {
    JsonValue root;
    std::vector<std::vector<std::byte>> buffers;
    std::vector<GltfBufferView> views;
    std::vector<GltfAccessor> accessors;
    std::filesystem::path sourcePath;
};

[[nodiscard]] std::vector<std::byte> load_uri_buffer(
    const std::filesystem::path& sourcePath,
    std::string_view uri) {
    constexpr std::string_view prefix = "data:";
    if (uri.starts_with(prefix)) {
        const std::size_t comma = uri.find(',');
        if (comma == std::string_view::npos) throw std::runtime_error("malformed data URI");
        const std::string_view metadata = uri.substr(prefix.size(), comma - prefix.size());
        if (!metadata.ends_with(";base64")) throw std::runtime_error("only base64 data URIs are supported");
        return decode_base64(uri.substr(comma + 1));
    }
    return read_binary_file(sourcePath.parent_path() / std::filesystem::path(uri));
}


struct JpegErrorState {
    jpeg_error_mgr manager{};
    jmp_buf jump{};
    char message[JMSG_LENGTH_MAX]{};
};

void jpeg_error_exit(j_common_ptr common) {
    auto* state = reinterpret_cast<JpegErrorState*>(common->err);
    (*common->err->format_message)(common, state->message);
    longjmp(state->jump, 1);
}

[[nodiscard]] ImportedImage decode_png_image(
    std::span<const std::byte> bytes,
    std::string name,
    const ModelImportOptions& options) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size())) {
        throw std::runtime_error("PNG header decode failed: " + std::string(image.message));
    }
    if (image.width == 0 || image.height == 0 || image.width > options.maximumImageDimension ||
        image.height > options.maximumImageDimension) {
        png_image_free(&image);
        throw std::runtime_error("PNG dimensions exceed configured limits");
    }
    image.format = PNG_FORMAT_RGBA;
    const png_alloc_size_t size = PNG_IMAGE_SIZE(image);
    if (size > options.maximumDecodedImageBytes) {
        png_image_free(&image);
        throw std::runtime_error("decoded PNG exceeds maximumDecodedImageBytes");
    }
    ImportedImage result;
    result.name = std::move(name);
    result.mimeType = "image/png";
    result.width = image.width;
    result.height = image.height;
    result.rgba8.resize(static_cast<std::size_t>(size));
    if (!png_image_finish_read(&image, nullptr, result.rgba8.data(), 0, nullptr)) {
        const std::string message = image.message;
        png_image_free(&image);
        throw std::runtime_error("PNG pixel decode failed: " + message);
    }
    png_image_free(&image);
    return result;
}

[[nodiscard]] ImportedImage decode_jpeg_image(
    std::span<const std::byte> bytes,
    std::string name,
    const ModelImportOptions& options) {
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<unsigned long>::max())) {
        throw std::runtime_error("JPEG input exceeds decoder range");
    }
    jpeg_decompress_struct decoder{};
    JpegErrorState error{};
    decoder.err = jpeg_std_error(&error.manager);
    error.manager.error_exit = jpeg_error_exit;
    if (setjmp(error.jump) != 0) {
        jpeg_destroy_decompress(&decoder);
        throw std::runtime_error("JPEG decode failed: " + std::string(error.message));
    }
    jpeg_create_decompress(&decoder);
    jpeg_mem_src(&decoder,
        reinterpret_cast<const unsigned char*>(bytes.data()),
        static_cast<unsigned long>(bytes.size()));
    jpeg_read_header(&decoder, TRUE);
    decoder.out_color_space = JCS_RGB;
    jpeg_start_decompress(&decoder);
    if (decoder.output_width == 0 || decoder.output_height == 0 ||
        decoder.output_width > options.maximumImageDimension ||
        decoder.output_height > options.maximumImageDimension) {
        jpeg_destroy_decompress(&decoder);
        throw std::runtime_error("JPEG dimensions exceed configured limits");
    }
    const std::uint64_t pixelBytes = static_cast<std::uint64_t>(decoder.output_width) *
        static_cast<std::uint64_t>(decoder.output_height) * 4ULL;
    if (pixelBytes > options.maximumDecodedImageBytes ||
        pixelBytes > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        jpeg_destroy_decompress(&decoder);
        throw std::runtime_error("decoded JPEG exceeds maximumDecodedImageBytes");
    }
    ImportedImage result;
    result.name = std::move(name);
    result.mimeType = "image/jpeg";
    result.width = decoder.output_width;
    result.height = decoder.output_height;
    result.rgba8.resize(static_cast<std::size_t>(pixelBytes));
    std::vector<unsigned char> row(static_cast<std::size_t>(decoder.output_width) * 3U);
    while (decoder.output_scanline < decoder.output_height) {
        JSAMPROW rowPointer = row.data();
        jpeg_read_scanlines(&decoder, &rowPointer, 1);
        const std::size_t y = static_cast<std::size_t>(decoder.output_scanline - 1U);
        for (std::size_t x = 0; x < decoder.output_width; ++x) {
            const std::size_t source = x * 3U;
            const std::size_t destination = (y * decoder.output_width + x) * 4U;
            result.rgba8[destination] = row[source];
            result.rgba8[destination + 1U] = row[source + 1U];
            result.rgba8[destination + 2U] = row[source + 2U];
            result.rgba8[destination + 3U] = 255U;
        }
    }
    jpeg_finish_decompress(&decoder);
    jpeg_destroy_decompress(&decoder);
    return result;
}

[[nodiscard]] ImportedImage decode_image_rgba(
    std::span<const std::byte> bytes,
    std::string_view mimeType,
    std::string name,
    const ModelImportOptions& options) {
    const bool pngSignature = bytes.size() >= 8 &&
        std::to_integer<std::uint8_t>(bytes[0]) == 0x89U &&
        std::to_integer<std::uint8_t>(bytes[1]) == 0x50U &&
        std::to_integer<std::uint8_t>(bytes[2]) == 0x4EU &&
        std::to_integer<std::uint8_t>(bytes[3]) == 0x47U;
    const bool jpegSignature = bytes.size() >= 2 &&
        std::to_integer<std::uint8_t>(bytes[0]) == 0xFFU &&
        std::to_integer<std::uint8_t>(bytes[1]) == 0xD8U;
    if (mimeType == "image/png" || (mimeType.empty() && pngSignature)) {
        return decode_png_image(bytes, std::move(name), options);
    }
    if (mimeType == "image/jpeg" || mimeType == "image/jpg" || (mimeType.empty() && jpegSignature)) {
        return decode_jpeg_image(bytes, std::move(name), options);
    }
    throw std::runtime_error("unsupported glTF image type; v1.6 supports PNG and JPEG");
}

[[nodiscard]] std::span<const std::byte> buffer_view_bytes(
    const GltfDocument& document,
    std::uint32_t viewIndex) {
    if (viewIndex >= document.views.size()) throw std::runtime_error("image bufferView index is invalid");
    const GltfBufferView& view = document.views[viewIndex];
    const std::vector<std::byte>& buffer = document.buffers.at(view.buffer);
    return std::span<const std::byte>(buffer.data() + static_cast<std::ptrdiff_t>(view.offset), view.length);
}

[[nodiscard]] GltfDocument load_gltf_document(const std::filesystem::path& path) {
    std::string jsonText;
    std::vector<std::byte> glbBinary;
    const std::string extension = path.extension().string();
    if (extension == ".glb" || extension == ".GLB") {
        const std::vector<std::byte> bytes = read_binary_file(path);
        if (bytes.size() < 20 || read_u32_le(bytes.data()) != 0x46546C67U) {
            throw std::runtime_error("invalid GLB header");
        }
        const std::uint32_t version = read_u32_le(bytes.data() + 4);
        const std::uint32_t declaredLength = read_u32_le(bytes.data() + 8);
        if (version != 2 || declaredLength != bytes.size()) throw std::runtime_error("unsupported or truncated GLB");
        std::size_t cursor = 12;
        bool foundJson = false;
        while (cursor + 8 <= bytes.size()) {
            const std::uint32_t chunkLength = read_u32_le(bytes.data() + cursor);
            const std::uint32_t chunkType = read_u32_le(bytes.data() + cursor + 4);
            cursor += 8;
            if (cursor + chunkLength > bytes.size()) throw std::runtime_error("GLB chunk exceeds file size");
            if (chunkType == 0x4E4F534AU) {
                if (foundJson) throw std::runtime_error("GLB contains multiple JSON chunks");
                jsonText.assign(reinterpret_cast<const char*>(bytes.data() + cursor), chunkLength);
                while (!jsonText.empty() && (jsonText.back() == '\0' || jsonText.back() == ' ')) jsonText.pop_back();
                foundJson = true;
            } else if (chunkType == 0x004E4942U && glbBinary.empty()) {
                glbBinary.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                                 bytes.begin() + static_cast<std::ptrdiff_t>(cursor + chunkLength));
            }
            cursor += chunkLength;
        }
        if (!foundJson) throw std::runtime_error("GLB JSON chunk is missing");
    } else {
        jsonText = read_text_file(path);
    }

    GltfDocument document{JsonParser(jsonText).parse(), {}, {}, {}, path};
    if (!document.root.is_object()) throw std::runtime_error("glTF root must be an object");

    const JsonValue* asset = document.root.find("asset");
    if (asset == nullptr || !asset->is_object() || json_string(asset->find("version")) != "2.0") {
        throw std::runtime_error("only glTF 2.0 is supported");
    }

    if (const JsonValue* required = document.root.find("extensionsRequired")) {
        if (!required->is_array()) throw std::runtime_error("extensionsRequired must be an array");
        if (!required->array().empty()) {
            throw std::runtime_error("required glTF extensions are not implemented in WP-7.1");
        }
    }

    const JsonValue* buffers = document.root.find("buffers");
    if (buffers != nullptr) {
        if (!buffers->is_array()) throw std::runtime_error("glTF buffers must be an array");
        document.buffers.reserve(buffers->array().size());
        for (std::size_t i = 0; i < buffers->array().size(); ++i) {
            const JsonValue& buffer = buffers->array()[i];
            if (!buffer.is_object()) throw std::runtime_error("glTF buffer must be an object");
            const JsonValue* uri = buffer.find("uri");
            std::vector<std::byte> bytes;
            if (uri != nullptr) bytes = load_uri_buffer(path, json_string(uri));
            else if (i == 0 && !glbBinary.empty()) bytes = glbBinary;
            else throw std::runtime_error("buffer without URI has no GLB BIN chunk");
            const std::size_t byteLength = json_size(buffer.find("byteLength"));
            if (bytes.size() < byteLength) throw std::runtime_error("buffer is shorter than declared byteLength");
            document.buffers.push_back(std::move(bytes));
        }
    }

    const JsonValue* views = document.root.find("bufferViews");
    if (views != nullptr) {
        if (!views->is_array()) throw std::runtime_error("bufferViews must be an array");
        for (const JsonValue& view : views->array()) {
            if (!view.is_object()) throw std::runtime_error("bufferView must be an object");
            GltfBufferView parsed;
            parsed.buffer = json_u32(view.find("buffer"));
            parsed.offset = json_size(view.find("byteOffset"));
            parsed.length = json_size(view.find("byteLength"));
            parsed.stride = json_size(view.find("byteStride"));
            if (parsed.buffer >= document.buffers.size() || parsed.offset + parsed.length > document.buffers[parsed.buffer].size()) {
                throw std::runtime_error("bufferView is outside its buffer");
            }
            document.views.push_back(parsed);
        }
    }

    const JsonValue* accessors = document.root.find("accessors");
    if (accessors != nullptr) {
        if (!accessors->is_array()) throw std::runtime_error("accessors must be an array");
        for (const JsonValue& accessor : accessors->array()) {
            if (!accessor.is_object()) throw std::runtime_error("accessor must be an object");
            if (accessor.find("sparse") != nullptr) throw std::runtime_error("sparse accessors are not implemented in WP-7.1");
            GltfAccessor parsed;
            if (const JsonValue* view = accessor.find("bufferView")) parsed.bufferView = json_u32(view);
            parsed.offset = json_size(accessor.find("byteOffset"));
            parsed.componentType = json_u32(accessor.find("componentType"));
            parsed.count = json_size(accessor.find("count"));
            parsed.components = component_count(json_string(accessor.find("type")));
            parsed.normalized = json_bool(accessor.find("normalized"));
            if (!parsed.bufferView.has_value()) throw std::runtime_error("accessor without bufferView is not implemented");
            if (*parsed.bufferView >= document.views.size()) throw std::runtime_error("accessor bufferView index is invalid");
            const GltfBufferView& view = document.views[*parsed.bufferView];
            const std::size_t elementSize = component_size(parsed.componentType) * parsed.components;
            const std::size_t stride = view.stride == 0 ? elementSize : view.stride;
            if (stride < elementSize) throw std::runtime_error("accessor stride is smaller than its element");
            if (parsed.count != 0) {
                const std::size_t required = parsed.offset + (parsed.count - 1U) * stride + elementSize;
                if (required > view.length) throw std::runtime_error("accessor exceeds bufferView");
            }
            document.accessors.push_back(parsed);
        }
    }
    return document;
}

[[nodiscard]] std::vector<float> unpack_accessor_floats(const GltfDocument& document, std::uint32_t index) {
    if (index >= document.accessors.size()) throw std::runtime_error("invalid accessor index");
    const GltfAccessor& accessor = document.accessors[index];
    const GltfBufferView& view = document.views[*accessor.bufferView];
    const std::vector<std::byte>& buffer = document.buffers[view.buffer];
    const std::size_t scalarSize = component_size(accessor.componentType);
    const std::size_t elementSize = scalarSize * accessor.components;
    const std::size_t stride = view.stride == 0 ? elementSize : view.stride;
    const std::byte* base = buffer.data() + view.offset + accessor.offset;
    std::vector<float> output(accessor.count * accessor.components);
    for (std::size_t element = 0; element < accessor.count; ++element) {
        for (std::uint32_t component = 0; component < accessor.components; ++component) {
            const double value = read_component(base + element * stride + component * scalarSize,
                                                accessor.componentType, accessor.normalized);
            if (value < -static_cast<double>(std::numeric_limits<float>::max()) ||
                value > static_cast<double>(std::numeric_limits<float>::max())) {
                throw std::runtime_error("accessor value is outside float range");
            }
            output[element * accessor.components + component] = static_cast<float>(value);
        }
    }
    return output;
}

[[nodiscard]] std::vector<std::uint32_t> unpack_accessor_indices(const GltfDocument& document, std::uint32_t index) {
    if (index >= document.accessors.size()) throw std::runtime_error("invalid index accessor");
    const GltfAccessor& accessor = document.accessors[index];
    if (accessor.components != 1 || accessor.normalized ||
        (accessor.componentType != 5121 && accessor.componentType != 5123 && accessor.componentType != 5125)) {
        throw std::runtime_error("glTF indices must be unsigned scalar values");
    }
    const GltfBufferView& view = document.views[*accessor.bufferView];
    const std::vector<std::byte>& buffer = document.buffers[view.buffer];
    const std::size_t scalarSize = component_size(accessor.componentType);
    const std::size_t stride = view.stride == 0 ? scalarSize : view.stride;
    const std::byte* base = buffer.data() + view.offset + accessor.offset;
    std::vector<std::uint32_t> output(accessor.count);
    for (std::size_t i = 0; i < accessor.count; ++i) {
        output[i] = static_cast<std::uint32_t>(read_component(base + i * stride, accessor.componentType, false));
    }
    return output;
}

[[nodiscard]] Matrix4 matrix_from_json_node(const JsonValue& node) {
    if (const JsonValue* matrix = node.find("matrix")) {
        const std::vector<float> values = json_float_array(matrix, 16);
        Matrix4 result;
        std::copy(values.begin(), values.end(), result.values.begin());
        return result;
    }
    Float3 translation{};
    Float3 scale{1.0F, 1.0F, 1.0F};
    Float4 rotation{0.0F, 0.0F, 0.0F, 1.0F};
    if (const JsonValue* value = node.find("translation")) {
        const std::vector<float> elements = json_float_array(value, 3);
        translation = {elements[0], elements[1], elements[2]};
    }
    if (const JsonValue* value = node.find("scale")) {
        const std::vector<float> elements = json_float_array(value, 3);
        scale = {elements[0], elements[1], elements[2]};
    }
    if (const JsonValue* value = node.find("rotation")) {
        const std::vector<float> elements = json_float_array(value, 4);
        rotation = {elements[0], elements[1], elements[2], elements[3]};
        const float norm = std::sqrt(rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z + rotation.w * rotation.w);
        if (!(norm > 0.0F)) throw std::runtime_error("node quaternion has zero length");
        rotation.x /= norm;
        rotation.y /= norm;
        rotation.z /= norm;
        rotation.w /= norm;
    }

    const float x = rotation.x;
    const float y = rotation.y;
    const float z = rotation.z;
    const float w = rotation.w;
    Matrix4 result = Matrix4::identity();
    result.values[0] = (1.0F - 2.0F * (y * y + z * z)) * scale.x;
    result.values[1] = (2.0F * (x * y + z * w)) * scale.x;
    result.values[2] = (2.0F * (x * z - y * w)) * scale.x;
    result.values[4] = (2.0F * (x * y - z * w)) * scale.y;
    result.values[5] = (1.0F - 2.0F * (x * x + z * z)) * scale.y;
    result.values[6] = (2.0F * (y * z + x * w)) * scale.y;
    result.values[8] = (2.0F * (x * z + y * w)) * scale.z;
    result.values[9] = (2.0F * (y * z - x * w)) * scale.z;
    result.values[10] = (1.0F - 2.0F * (x * x + y * y)) * scale.z;
    result.values[12] = translation.x;
    result.values[13] = translation.y;
    result.values[14] = translation.z;
    return result;
}

void update_world_transform(ImportedScene& scene, std::uint32_t nodeIndex, const Matrix4& parent, std::vector<std::uint8_t>& marks) {
    if (nodeIndex >= scene.nodes.size()) throw std::runtime_error("scene root or child node index is invalid");
    if (marks[nodeIndex] == 1) throw std::runtime_error("cycle detected in glTF node hierarchy");
    if (marks[nodeIndex] == 2) return;
    marks[nodeIndex] = 1;
    ImportedNode& node = scene.nodes[nodeIndex];
    node.worldTransform = multiply(parent, node.localTransform);
    for (std::uint32_t child : node.children) {
        if (child >= scene.nodes.size()) throw std::runtime_error("node child index is invalid");
        if (scene.nodes[child].parent.has_value() && *scene.nodes[child].parent != nodeIndex) {
            throw std::runtime_error("glTF node has more than one parent");
        }
        scene.nodes[child].parent = nodeIndex;
        update_world_transform(scene, child, node.worldTransform, marks);
    }
    marks[nodeIndex] = 2;
}

[[nodiscard]] ImportedModelResult import_gltf(const std::filesystem::path& path, const ModelImportOptions& options) {
    ImportedModelResult result;
    try {
        const GltfDocument document = load_gltf_document(path);
        const JsonValue& root = document.root;

        result.scene.name = path.stem().string();

        if (const JsonValue* samplers = root.find("samplers")) {
            if (!samplers->is_array()) throw std::runtime_error("samplers must be an array");
            result.scene.samplers.reserve(samplers->array().size());
            for (const JsonValue& samplerValue : samplers->array()) {
                if (!samplerValue.is_object()) throw std::runtime_error("sampler must be an object");
                ImportedSampler sampler;
                const auto parse_wrap = [](std::uint32_t value) {
                    if (value == 33071U) return ImportedWrapMode::ClampToEdge;
                    if (value == 33648U) return ImportedWrapMode::MirroredRepeat;
                    if (value == 10497U) return ImportedWrapMode::Repeat;
                    throw std::runtime_error("unsupported glTF texture wrap mode");
                };
                const auto parse_filter = [](std::uint32_t value) {
                    // Mipmap modes collapse to their base nearest/linear filter because the
                    // offline cooker samples the highest-resolution source image.
                    if (value == 9728U || value == 9984U || value == 9986U) return ImportedTextureFilter::Nearest;
                    if (value == 9729U || value == 9985U || value == 9987U) return ImportedTextureFilter::Linear;
                    throw std::runtime_error("unsupported glTF texture filter");
                };
                sampler.wrapS = parse_wrap(json_u32(samplerValue.find("wrapS"), 10497U));
                sampler.wrapT = parse_wrap(json_u32(samplerValue.find("wrapT"), 10497U));
                sampler.minFilter = parse_filter(json_u32(samplerValue.find("minFilter"), 9729U));
                sampler.magFilter = parse_filter(json_u32(samplerValue.find("magFilter"), 9729U));
                result.scene.samplers.push_back(sampler);
            }
        }

        if (const JsonValue* images = root.find("images")) {
            if (!images->is_array()) throw std::runtime_error("images must be an array");
            result.scene.images.reserve(images->array().size());
            std::uint64_t totalDecodedBytes = 0;
            for (std::size_t index = 0; index < images->array().size(); ++index) {
                const JsonValue& imageValue = images->array()[index];
                if (!imageValue.is_object()) throw std::runtime_error("image must be an object");
                const std::string name = json_string(imageValue.find("name"), "Image_" + std::to_string(index));
                const std::string mime = json_string(imageValue.find("mimeType"));
                std::vector<std::byte> owned;
                std::span<const std::byte> encoded;
                if (const JsonValue* uri = imageValue.find("uri")) {
                    owned = load_uri_buffer(path, json_string(uri));
                    encoded = owned;
                } else if (const JsonValue* view = imageValue.find("bufferView")) {
                    encoded = buffer_view_bytes(document, json_u32(view));
                } else {
                    throw std::runtime_error("glTF image requires uri or bufferView");
                }
                ImportedImage decoded = decode_image_rgba(encoded, mime, name, options);
                totalDecodedBytes += decoded.rgba8.size();
                if (totalDecodedBytes > options.maximumDecodedImageBytes) {
                    throw std::runtime_error("combined decoded images exceed maximumDecodedImageBytes");
                }
                result.scene.images.push_back(std::move(decoded));
            }
        }

        if (const JsonValue* textures = root.find("textures")) {
            if (!textures->is_array()) throw std::runtime_error("textures must be an array");
            result.scene.textures.reserve(textures->array().size());
            for (std::size_t index = 0; index < textures->array().size(); ++index) {
                const JsonValue& textureValue = textures->array()[index];
                if (!textureValue.is_object()) throw std::runtime_error("texture must be an object");
                ImportedTexture texture;
                texture.name = json_string(textureValue.find("name"), "Texture_" + std::to_string(index));
                texture.imageIndex = json_u32(textureValue.find("source"));
                if (texture.imageIndex >= result.scene.images.size()) throw std::runtime_error("texture source image is invalid");
                if (const JsonValue* sampler = textureValue.find("sampler")) {
                    texture.samplerIndex = json_u32(sampler);
                    if (*texture.samplerIndex >= result.scene.samplers.size()) throw std::runtime_error("texture sampler is invalid");
                }
                result.scene.textures.push_back(std::move(texture));
            }
        }

        const JsonValue* materials = root.find("materials");
        if (materials != nullptr) {
            if (!materials->is_array()) throw std::runtime_error("materials must be an array");
            if (materials->array().size() > options.maximumMaterials) {
                throw std::runtime_error("glTF material count exceeds the DVE material limit");
            }
            for (const JsonValue& materialValue : materials->array()) {
                if (!materialValue.is_object()) throw std::runtime_error("material must be an object");
                ImportedMaterial material;
                material.name = json_string(materialValue.find("name"), "Material");
                if (const JsonValue* pbr = materialValue.find("pbrMetallicRoughness")) {
                    if (!pbr->is_object()) throw std::runtime_error("pbrMetallicRoughness must be an object");
                    if (const JsonValue* color = pbr->find("baseColorFactor")) {
                        const std::vector<float> values = json_float_array(color, 4);
                        material.baseColorFactor = {values[0], values[1], values[2], values[3]};
                    }
                    material.metallicFactor = json_float(pbr->find("metallicFactor"), 1.0F);
                    material.roughnessFactor = json_float(pbr->find("roughnessFactor"), 1.0F);
                    const auto parse_texture_info = [&](const JsonValue* texture, const char* label,
                                                        std::optional<std::uint32_t>& destination,
                                                        std::uint32_t& texcoord) {
                        if (texture == nullptr) return;
                        if (!texture->is_object()) throw std::runtime_error(std::string(label) + " must be an object");
                        destination = json_u32(texture->find("index"));
                        texcoord = json_u32(texture->find("texCoord"), 0U);
                        if (*destination >= result.scene.textures.size()) {
                            throw std::runtime_error(std::string(label) + " index is invalid");
                        }
                        if (texcoord > 1U) {
                            result.diagnostics.push_back({ImportDiagnostic::Severity::Warning,
                                "GLTF_TEXCOORD_UNSUPPORTED",
                                std::string(label) + " requests TEXCOORD_" + std::to_string(texcoord) +
                                "; DVE currently preserves sets 0 and 1 only."});
                        }
                    };
                    parse_texture_info(pbr->find("baseColorTexture"), "baseColorTexture",
                                       material.baseColorTexture, material.baseColorTexcoord);
                    parse_texture_info(pbr->find("metallicRoughnessTexture"), "metallicRoughnessTexture",
                                       material.metallicRoughnessTexture, material.metallicRoughnessTexcoord);
                }
                if (const JsonValue* emissive = materialValue.find("emissiveFactor")) {
                    const std::vector<float> values = json_float_array(emissive, 3);
                    material.emissiveFactor = {values[0], values[1], values[2]};
                }
                const auto parse_material_texture = [&](const JsonValue* texture, const char* label,
                                                        std::optional<std::uint32_t>& destination,
                                                        std::uint32_t& texcoord) {
                    if (texture == nullptr) return;
                    if (!texture->is_object()) throw std::runtime_error(std::string(label) + " must be an object");
                    destination = json_u32(texture->find("index"));
                    texcoord = json_u32(texture->find("texCoord"), 0U);
                    if (*destination >= result.scene.textures.size()) {
                        throw std::runtime_error(std::string(label) + " index is invalid");
                    }
                    if (texcoord > 1U) {
                        result.diagnostics.push_back({ImportDiagnostic::Severity::Warning,
                            "GLTF_TEXCOORD_UNSUPPORTED",
                            std::string(label) + " requests a texture coordinate set above 1."});
                    }
                };
                if (const JsonValue* normal = materialValue.find("normalTexture")) {
                    parse_material_texture(normal, "normalTexture", material.normalTexture, material.normalTexcoord);
                    material.normalScale = json_float(normal->find("scale"), 1.0F);
                }
                parse_material_texture(materialValue.find("emissiveTexture"), "emissiveTexture",
                                       material.emissiveTexture, material.emissiveTexcoord);
                // Dedicated opacity is a DVE extension; standard glTF opacity remains base-color alpha.
                if (const JsonValue* extensions = materialValue.find("extensions")) {
                    if (extensions->is_object()) {
                        if (const JsonValue* channels = extensions->find("DVE_material_channels")) {
                            if (!channels->is_object()) throw std::runtime_error("DVE_material_channels must be an object");
                            parse_material_texture(channels->find("opacityTexture"), "opacityTexture",
                                                   material.opacityTexture, material.opacityTexcoord);
                        }
                    }
                }
                const std::string alpha = json_string(materialValue.find("alphaMode"), "OPAQUE");
                if (alpha == "OPAQUE") material.alphaMode = ImportedAlphaMode::Opaque;
                else if (alpha == "MASK") material.alphaMode = ImportedAlphaMode::Mask;
                else if (alpha == "BLEND") material.alphaMode = ImportedAlphaMode::Blend;
                else throw std::runtime_error("unsupported glTF alphaMode");
                material.alphaCutoff = json_float(materialValue.find("alphaCutoff"), 0.5F);
                material.doubleSided = json_bool(materialValue.find("doubleSided"));
                result.scene.materials.push_back(std::move(material));
            }
        }
        if (result.scene.materials.empty()) {
            ImportedMaterial defaultMaterial;
            defaultMaterial.name = "Default";
            defaultMaterial.metallicFactor = 0.0F;
            defaultMaterial.roughnessFactor = 1.0F;
            result.scene.materials.push_back(std::move(defaultMaterial));
        }

        const JsonValue* meshes = root.find("meshes");
        if (meshes != nullptr) {
            if (!meshes->is_array()) throw std::runtime_error("meshes must be an array");
            result.scene.meshes.reserve(meshes->array().size());
            for (std::size_t meshIndex = 0; meshIndex < meshes->array().size(); ++meshIndex) {
                const JsonValue& meshValue = meshes->array()[meshIndex];
                if (!meshValue.is_object()) throw std::runtime_error("mesh must be an object");
                ImportedMesh mesh;
                mesh.name = json_string(meshValue.find("name"), "Mesh_" + std::to_string(meshIndex));
                const JsonValue* primitives = meshValue.find("primitives");
                if (primitives == nullptr || !primitives->is_array()) throw std::runtime_error("mesh primitives are missing");
                std::uint32_t primitiveIndex = 0;
                for (const JsonValue& primitive : primitives->array()) {
                    if (!primitive.is_object()) throw std::runtime_error("mesh primitive must be an object");
                    const std::uint32_t mode = json_u32(primitive.find("mode"), 4);
                    if (mode != 4) {
                        if (options.strict) throw std::runtime_error("non-triangle primitive encountered in strict mode");
                        result.diagnostics.push_back({ImportDiagnostic::Severity::Warning,
                            "GLTF_PRIMITIVE_MODE",
                            "Skipped non-triangle primitive in mesh " + mesh.name + "."});
                        ++primitiveIndex;
                        continue;
                    }
                    const JsonValue* attributes = primitive.find("attributes");
                    if (attributes == nullptr || !attributes->is_object()) throw std::runtime_error("primitive attributes are missing");
                    const JsonValue* positionAccessor = attributes->find("POSITION");
                    if (positionAccessor == nullptr) throw std::runtime_error("primitive POSITION attribute is missing");
                    const std::uint32_t positionIndex = json_u32(positionAccessor);
                    const GltfAccessor& positionMeta = document.accessors.at(positionIndex);
                    if (positionMeta.components != 3) throw std::runtime_error("POSITION accessor must be VEC3");
                    const std::vector<float> positions = unpack_accessor_floats(document, positionIndex);
                    const std::size_t vertexCount = positionMeta.count;
                    std::vector<float> normals;
                    std::vector<float> texcoords;
                    std::vector<float> texcoords1;
                    std::vector<float> colors;
                    std::uint32_t colorComponents = 0;
                    if (const JsonValue* accessor = attributes->find("NORMAL")) {
                        const std::uint32_t accessorIndex = json_u32(accessor);
                        if (document.accessors.at(accessorIndex).count != vertexCount ||
                            document.accessors.at(accessorIndex).components != 3) {
                            throw std::runtime_error("NORMAL accessor does not match POSITION");
                        }
                        normals = unpack_accessor_floats(document, accessorIndex);
                    }
                    if (const JsonValue* accessor = attributes->find("TEXCOORD_0")) {
                        const std::uint32_t accessorIndex = json_u32(accessor);
                        if (document.accessors.at(accessorIndex).count != vertexCount ||
                            document.accessors.at(accessorIndex).components != 2) {
                            throw std::runtime_error("TEXCOORD_0 accessor does not match POSITION");
                        }
                        texcoords = unpack_accessor_floats(document, accessorIndex);
                    }
                    if (const JsonValue* accessor = attributes->find("TEXCOORD_1")) {
                        const std::uint32_t accessorIndex = json_u32(accessor);
                        if (document.accessors.at(accessorIndex).count != vertexCount ||
                            document.accessors.at(accessorIndex).components != 2) {
                            throw std::runtime_error("TEXCOORD_1 accessor does not match POSITION");
                        }
                        texcoords1 = unpack_accessor_floats(document, accessorIndex);
                    }
                    if (const JsonValue* accessor = attributes->find("COLOR_0")) {
                        const std::uint32_t accessorIndex = json_u32(accessor);
                        const GltfAccessor& metadata = document.accessors.at(accessorIndex);
                        if (metadata.count != vertexCount || (metadata.components != 3 && metadata.components != 4)) {
                            throw std::runtime_error("COLOR_0 accessor does not match POSITION");
                        }
                        colorComponents = metadata.components;
                        colors = unpack_accessor_floats(document, accessorIndex);
                    }

                    const std::uint32_t baseVertex = static_cast<std::uint32_t>(mesh.vertices.size());
                    if (mesh.vertices.size() + vertexCount > std::numeric_limits<std::uint32_t>::max()) {
                        throw std::runtime_error("mesh exceeds uint32 vertex limit");
                    }
                    mesh.vertices.reserve(mesh.vertices.size() + vertexCount);
                    for (std::size_t i = 0; i < vertexCount; ++i) {
                        ImportedVertex vertex;
                        vertex.position = {positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]};
                        if (!normals.empty()) vertex.normal = {normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2]};
                        if (!texcoords.empty()) vertex.texcoord = {texcoords[i * 2], texcoords[i * 2 + 1]};
                        if (!texcoords1.empty()) vertex.texcoord1 = {texcoords1[i * 2], texcoords1[i * 2 + 1]};
                        if (!colors.empty()) {
                            vertex.color = {colors[i * colorComponents], colors[i * colorComponents + 1],
                                colors[i * colorComponents + 2], colorComponents == 4 ? colors[i * 4 + 3] : 1.0F};
                        }
                        mesh.vertices.push_back(vertex);
                    }

                    std::vector<std::uint32_t> indices;
                    if (const JsonValue* accessor = primitive.find("indices")) {
                        indices = unpack_accessor_indices(document, json_u32(accessor));
                    } else {
                        indices.resize(vertexCount);
                        for (std::size_t i = 0; i < vertexCount; ++i) indices[i] = static_cast<std::uint32_t>(i);
                    }
                    if (indices.size() % 3U != 0) throw std::runtime_error("triangle index count is not divisible by three");
                    const std::uint32_t sourceMaterial = json_u32(primitive.find("material"), 0);
                    const std::uint32_t materialIndex = std::min<std::uint32_t>(sourceMaterial,
                        static_cast<std::uint32_t>(result.scene.materials.size() - 1U));
                    for (std::size_t i = 0; i < indices.size(); i += 3) {
                        if (indices[i] >= vertexCount || indices[i + 1] >= vertexCount || indices[i + 2] >= vertexCount) {
                            throw std::runtime_error("primitive index exceeds vertex count");
                        }
                        mesh.triangles.push_back({{
                            baseVertex + indices[i], baseVertex + indices[i + 1], baseVertex + indices[i + 2]},
                            materialIndex, primitiveIndex});
                    }
                    ++primitiveIndex;
                }
                result.scene.meshes.push_back(std::move(mesh));
            }
        }

        const JsonValue* nodes = root.find("nodes");
        if (nodes != nullptr) {
            if (!nodes->is_array()) throw std::runtime_error("nodes must be an array");
            result.scene.nodes.reserve(nodes->array().size());
            for (std::size_t index = 0; index < nodes->array().size(); ++index) {
                const JsonValue& nodeValue = nodes->array()[index];
                if (!nodeValue.is_object()) throw std::runtime_error("node must be an object");
                ImportedNode node;
                node.name = json_string(nodeValue.find("name"), "Node_" + std::to_string(index));
                node.localTransform = matrix_from_json_node(nodeValue);
                if (const JsonValue* mesh = nodeValue.find("mesh")) {
                    const std::uint32_t meshIndex = json_u32(mesh);
                    if (meshIndex >= result.scene.meshes.size()) throw std::runtime_error("node mesh index is invalid");
                    node.mesh = meshIndex;
                }
                if (const JsonValue* children = nodeValue.find("children")) {
                    if (!children->is_array()) throw std::runtime_error("node children must be an array");
                    for (const JsonValue& child : children->array()) node.children.push_back(json_u32(&child));
                }
                if (const JsonValue* extras = nodeValue.find("extras")) {
                    // The generic parser validates extras. The first implementation records its presence;
                    // canonical reserialization is intentionally deferred.
                    node.extrasJson = canonical_json(*extras);
                }
                result.scene.nodes.push_back(std::move(node));
            }
        }

        const JsonValue* scenes = root.find("scenes");
        if (scenes != nullptr && scenes->is_array() && !scenes->array().empty()) {
            std::uint32_t selected = json_u32(root.find("scene"), 0);
            if (selected >= scenes->array().size()) throw std::runtime_error("selected scene index is invalid");
            const JsonValue& sceneValue = scenes->array()[selected];
            if (!sceneValue.is_object()) throw std::runtime_error("scene must be an object");
            result.scene.name = json_string(sceneValue.find("name"), result.scene.name);
            if (const JsonValue* roots = sceneValue.find("nodes")) {
                if (!roots->is_array()) throw std::runtime_error("scene nodes must be an array");
                for (const JsonValue& rootNode : roots->array()) result.scene.roots.push_back(json_u32(&rootNode));
            }
        } else {
            std::vector<bool> isChild(result.scene.nodes.size(), false);
            for (const ImportedNode& node : result.scene.nodes) {
                for (std::uint32_t child : node.children) {
                    if (child >= isChild.size()) throw std::runtime_error("node child index is invalid");
                    isChild[child] = true;
                }
            }
            for (std::size_t i = 0; i < isChild.size(); ++i) {
                if (!isChild[i]) result.scene.roots.push_back(static_cast<std::uint32_t>(i));
            }
        }

        if (result.scene.nodes.empty() && !result.scene.meshes.empty()) {
            result.scene.nodes.reserve(result.scene.meshes.size());
            for (std::size_t i = 0; i < result.scene.meshes.size(); ++i) {
                ImportedNode node;
                node.name = result.scene.meshes[i].name;
                node.mesh = static_cast<std::uint32_t>(i);
                result.scene.roots.push_back(static_cast<std::uint32_t>(i));
                result.scene.nodes.push_back(std::move(node));
            }
        }

        std::vector<std::uint8_t> marks(result.scene.nodes.size(), 0);
        for (std::uint32_t rootNode : result.scene.roots) update_world_transform(result.scene, rootNode, Matrix4::identity(), marks);
        for (std::size_t i = 0; i < marks.size(); ++i) {
            if (marks[i] == 0) {
                result.diagnostics.push_back({ImportDiagnostic::Severity::Warning, "GLTF_UNREACHABLE_NODE",
                    "Node " + result.scene.nodes[i].name + " is not reachable from the selected scene and was ignored."});
                result.scene.nodes[i].mesh.reset();
            }
        }
        result.success = true;
    } catch (const std::exception& exception) {
        result.diagnostics.push_back({ImportDiagnostic::Severity::Error, "GLTF_IMPORT", exception.what()});
        result.success = false;
    }
    return result;
}

[[nodiscard]] ImportedModelResult import_obj(const std::filesystem::path& path, const ModelImportOptions&) {
    ImportedModelResult result;
    try {
        std::ifstream input(path);
        if (!input) throw std::runtime_error("unable to open OBJ file");
        std::vector<Float3> positions;
        std::vector<Float3> normals;
        std::vector<Float2> texcoords;
        ImportedMesh mesh;
        mesh.name = path.stem().string();
        std::map<std::tuple<int, int, int>, std::uint32_t> vertexMap;

        auto resolve_index = [](int index, std::size_t size) -> std::size_t {
            if (index > 0) return static_cast<std::size_t>(index - 1);
            if (index < 0) return size - static_cast<std::size_t>(-index);
            throw std::runtime_error("OBJ indices are one-based and cannot be zero");
        };

        std::string line;
        while (std::getline(input, line)) {
            std::istringstream stream(line);
            std::string command;
            stream >> command;
            if (command.empty() || command[0] == '#') continue;
            if (command == "v") {
                Float3 value;
                if (!(stream >> value.x >> value.y >> value.z)) throw std::runtime_error("invalid OBJ vertex");
                positions.push_back(value);
            } else if (command == "vn") {
                Float3 value;
                if (!(stream >> value.x >> value.y >> value.z)) throw std::runtime_error("invalid OBJ normal");
                normals.push_back(value);
            } else if (command == "vt") {
                Float2 value;
                if (!(stream >> value.x >> value.y)) throw std::runtime_error("invalid OBJ texcoord");
                texcoords.push_back(value);
            } else if (command == "f") {
                std::vector<std::uint32_t> polygon;
                std::string token;
                while (stream >> token) {
                    int positionIndex = 0;
                    int texcoordIndex = 0;
                    int normalIndex = 0;
                    const std::size_t firstSlash = token.find('/');
                    const std::size_t secondSlash = firstSlash == std::string::npos ? std::string::npos : token.find('/', firstSlash + 1);
                    positionIndex = std::stoi(token.substr(0, firstSlash));
                    if (firstSlash != std::string::npos && secondSlash != firstSlash + 1) {
                        texcoordIndex = std::stoi(token.substr(firstSlash + 1, secondSlash - firstSlash - 1));
                    }
                    if (secondSlash != std::string::npos && secondSlash + 1 < token.size()) {
                        normalIndex = std::stoi(token.substr(secondSlash + 1));
                    }
                    const auto key = std::tuple{positionIndex, texcoordIndex, normalIndex};
                    auto it = vertexMap.find(key);
                    if (it == vertexMap.end()) {
                        ImportedVertex vertex;
                        vertex.position = positions.at(resolve_index(positionIndex, positions.size()));
                        if (texcoordIndex != 0) vertex.texcoord = texcoords.at(resolve_index(texcoordIndex, texcoords.size()));
                        if (normalIndex != 0) vertex.normal = normals.at(resolve_index(normalIndex, normals.size()));
                        const std::uint32_t newIndex = static_cast<std::uint32_t>(mesh.vertices.size());
                        mesh.vertices.push_back(vertex);
                        it = vertexMap.emplace(key, newIndex).first;
                    }
                    polygon.push_back(it->second);
                }
                if (polygon.size() < 3) throw std::runtime_error("OBJ face has fewer than three vertices");
                for (std::size_t i = 1; i + 1 < polygon.size(); ++i) {
                    mesh.triangles.push_back({{polygon[0], polygon[i], polygon[i + 1]}, 0, 0});
                }
            }
        }
        ImportedMaterial material;
        material.name = "OBJ_Default";
        material.metallicFactor = 0.0F;
        result.scene.name = path.stem().string();
        result.scene.materials.push_back(std::move(material));
        result.scene.meshes.push_back(std::move(mesh));
        ImportedNode node;
        node.name = result.scene.meshes.front().name;
        node.mesh = 0;
        result.scene.nodes.push_back(std::move(node));
        result.scene.roots.push_back(0);
        result.success = true;
        result.diagnostics.push_back({ImportDiagnostic::Severity::Warning, "OBJ_MATERIAL_LIMITED",
            "The WP-7.1 OBJ fallback imports geometry but does not yet parse MTL materials."});
    } catch (const std::exception& exception) {
        result.diagnostics.push_back({ImportDiagnostic::Severity::Error, "OBJ_IMPORT", exception.what()});
        result.success = false;
    }
    return result;
}

[[nodiscard]] Float3 cross(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] Float3 sub(Float3 a, Float3 b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] Float3 add3(Float3 a, Float3 b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] Float3 mul(Float3 a, float scalar) noexcept {
    return {a.x * scalar, a.y * scalar, a.z * scalar};
}

[[nodiscard]] float dot3(Float3 a, Float3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] float length_squared3(Float3 value) noexcept { return dot3(value, value); }

[[nodiscard]] bool separating_axis(Float3 axis, const std::array<Float3, 3>& triangle, Float3 halfExtent) noexcept {
    const float axisLengthSq = length_squared3(axis);
    if (axisLengthSq <= kAxisEpsilonSquared) return false;
    const float p0 = dot3(triangle[0], axis);
    const float p1 = dot3(triangle[1], axis);
    const float p2 = dot3(triangle[2], axis);
    const float minimum = std::min({p0, p1, p2});
    const float maximum = std::max({p0, p1, p2});
    const float radius = halfExtent.x * std::abs(axis.x) + halfExtent.y * std::abs(axis.y) + halfExtent.z * std::abs(axis.z);
    return minimum > radius || maximum < -radius;
}

[[nodiscard]] bool triangle_intersects_box(
    const std::array<Float3, 3>& worldTriangle,
    Float3 boxCenter,
    Float3 halfExtent) noexcept {
    std::array<Float3, 3> triangle{
        sub(worldTriangle[0], boxCenter),
        sub(worldTriangle[1], boxCenter),
        sub(worldTriangle[2], boxCenter),
    };
    const std::array<Float3, 3> edges{
        sub(triangle[1], triangle[0]),
        sub(triangle[2], triangle[1]),
        sub(triangle[0], triangle[2]),
    };
    const std::array<Float3, 3> boxAxes{{{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}}};
    for (Float3 axis : boxAxes) if (separating_axis(axis, triangle, halfExtent)) return false;
    const Float3 normal = cross(edges[0], edges[1]);
    if (separating_axis(normal, triangle, halfExtent)) return false;
    for (Float3 edge : edges) {
        for (Float3 axis : boxAxes) {
            if (separating_axis(cross(edge, axis), triangle, halfExtent)) return false;
        }
    }
    return true;
}

[[nodiscard]] Float3 closest_point_on_triangle(Float3 point, Float3 a, Float3 b, Float3 c) noexcept {
    // Real-Time Collision Detection, Christer Ericson, section 5.1.5.
    const Float3 ab = sub(b, a);
    const Float3 ac = sub(c, a);
    const Float3 ap = sub(point, a);
    const float d1 = dot3(ab, ap);
    const float d2 = dot3(ac, ap);
    if (d1 <= 0.0F && d2 <= 0.0F) return a;

    const Float3 bp = sub(point, b);
    const float d3 = dot3(ab, bp);
    const float d4 = dot3(ac, bp);
    if (d3 >= 0.0F && d4 <= d3) return b;

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) {
        const float v = d1 / (d1 - d3);
        return add3(a, mul(ab, v));
    }

    const Float3 cp = sub(point, c);
    const float d5 = dot3(ab, cp);
    const float d6 = dot3(ac, cp);
    if (d6 >= 0.0F && d5 <= d6) return c;

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) {
        const float w = d2 / (d2 - d6);
        return add3(a, mul(ac, w));
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F) {
        const Float3 bc = sub(c, b);
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return add3(b, mul(bc, w));
    }

    const float inverse = 1.0F / (va + vb + vc);
    const float v = vb * inverse;
    const float w = vc * inverse;
    return add3(a, add3(mul(ab, v), mul(ac, w)));
}

constexpr std::uint32_t kNoSourceMaterial = std::numeric_limits<std::uint32_t>::max();

[[nodiscard]] float srgb_to_linear(float value) noexcept {
    value = std::clamp(value, 0.0F, 1.0F);
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] std::uint8_t float_to_unorm8(float value) noexcept {
    return static_cast<std::uint8_t>(std::clamp(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F), 0L, 255L));
}

[[nodiscard]] std::uint32_t pack_rgba8(Float4 color) noexcept {
    return static_cast<std::uint32_t>(float_to_unorm8(color.x)) |
        (static_cast<std::uint32_t>(float_to_unorm8(color.y)) << 8U) |
        (static_cast<std::uint32_t>(float_to_unorm8(color.z)) << 16U) |
        (static_cast<std::uint32_t>(float_to_unorm8(color.w)) << 24U);
}

[[nodiscard]] Float4 unpack_rgba8(std::uint32_t packed) noexcept {
    constexpr float inverse = 1.0F / 255.0F;
    return {
        static_cast<float>(packed & 0xFFU) * inverse,
        static_cast<float>((packed >> 8U) & 0xFFU) * inverse,
        static_cast<float>((packed >> 16U) & 0xFFU) * inverse,
        static_cast<float>((packed >> 24U) & 0xFFU) * inverse,
    };
}

[[nodiscard]] std::uint64_t material_sample_key(std::uint32_t sourceMaterial, std::uint32_t rgba8) noexcept {
    return (static_cast<std::uint64_t>(sourceMaterial) << 32U) | rgba8;
}

struct SurfaceCell {
    std::uint32_t sourceMaterial{kNoSourceMaterial};
    std::uint32_t rgba8{};
    float distanceSquared{std::numeric_limits<float>::infinity()};
    bool surface{};
    bool candidateSeen{};

    [[nodiscard]] bool occupied() const noexcept { return sourceMaterial != kNoSourceMaterial; }
    [[nodiscard]] std::uint64_t key() const noexcept { return material_sample_key(sourceMaterial, rgba8); }
};

class DenseCookGrid {
public:
    DenseCookGrid(Int3 minimum, Int3 maximum, std::uint64_t maximumVoxels)
        : minimum_(minimum), maximum_(maximum) {
        const std::int64_t sx = static_cast<std::int64_t>(maximum.x) - minimum.x + 1;
        const std::int64_t sy = static_cast<std::int64_t>(maximum.y) - minimum.y + 1;
        const std::int64_t sz = static_cast<std::int64_t>(maximum.z) - minimum.z + 1;
        if (sx <= 0 || sy <= 0 || sz <= 0) throw std::runtime_error("invalid cooking grid bounds");
        const std::uint64_t count = static_cast<std::uint64_t>(sx) * static_cast<std::uint64_t>(sy) * static_cast<std::uint64_t>(sz);
        if (count > maximumVoxels) {
            throw std::runtime_error("working voxel grid exceeds maximumWorkingVoxels (" + std::to_string(count) + ")");
        }
        size_ = {static_cast<std::int32_t>(sx), static_cast<std::int32_t>(sy), static_cast<std::int32_t>(sz)};
        cells_.resize(static_cast<std::size_t>(count));
    }

    [[nodiscard]] Int3 minimum() const noexcept { return minimum_; }
    [[nodiscard]] Int3 maximum() const noexcept { return maximum_; }
    [[nodiscard]] Int3 size() const noexcept { return size_; }
    [[nodiscard]] std::size_t count() const noexcept { return cells_.size(); }

    [[nodiscard]] bool contains(Int3 coordinate) const noexcept {
        return coordinate.x >= minimum_.x && coordinate.x <= maximum_.x &&
               coordinate.y >= minimum_.y && coordinate.y <= maximum_.y &&
               coordinate.z >= minimum_.z && coordinate.z <= maximum_.z;
    }

    [[nodiscard]] std::size_t index(Int3 coordinate) const noexcept {
        const std::size_t x = static_cast<std::size_t>(coordinate.x - minimum_.x);
        const std::size_t y = static_cast<std::size_t>(coordinate.y - minimum_.y);
        const std::size_t z = static_cast<std::size_t>(coordinate.z - minimum_.z);
        return x + static_cast<std::size_t>(size_.x) * (y + static_cast<std::size_t>(size_.y) * z);
    }

    [[nodiscard]] Int3 coordinate(std::size_t index) const noexcept {
        const std::size_t xy = static_cast<std::size_t>(size_.x) * static_cast<std::size_t>(size_.y);
        const std::size_t z = index / xy;
        const std::size_t rem = index % xy;
        const std::size_t y = rem / static_cast<std::size_t>(size_.x);
        const std::size_t x = rem % static_cast<std::size_t>(size_.x);
        return {minimum_.x + static_cast<std::int32_t>(x), minimum_.y + static_cast<std::int32_t>(y),
                minimum_.z + static_cast<std::int32_t>(z)};
    }

    SurfaceCell& cell(Int3 coordinate) noexcept { return cells_[index(coordinate)]; }
    const SurfaceCell& cell(Int3 coordinate) const noexcept { return cells_[index(coordinate)]; }
    SurfaceCell& cell(std::size_t index) noexcept { return cells_[index]; }
    const SurfaceCell& cell(std::size_t index) const noexcept { return cells_[index]; }

private:
    Int3 minimum_{};
    Int3 maximum_{};
    Int3 size_{};
    std::vector<SurfaceCell> cells_;
};

struct WorldTriangle {
    std::array<Float3, 3> vertices{};
    std::array<Float2, 3> texcoords{};
    std::array<Float4, 3> colors{};
    std::uint32_t sourceMaterial{};
};

struct MeshTopologySummary {
    std::uint64_t boundaryEdges{};
    std::uint64_t nonManifoldEdges{};
    std::uint64_t islands{};
};

[[nodiscard]] MeshTopologySummary analyze_mesh_topology(const ImportedMesh& mesh) {
    MeshTopologySummary summary;
    if (mesh.vertices.empty() || mesh.triangles.empty()) return summary;

    std::vector<std::uint32_t> parent(mesh.vertices.size());
    std::vector<std::uint8_t> used(mesh.vertices.size(), 0);
    for (std::uint32_t index = 0; index < parent.size(); ++index) parent[index] = index;
    const auto find_root = [&](std::uint32_t value, auto&& self) -> std::uint32_t {
        if (parent[value] != value) parent[value] = self(parent[value], self);
        return parent[value];
    };
    const auto unite = [&](std::uint32_t a, std::uint32_t b) {
        a = find_root(a, find_root);
        b = find_root(b, find_root);
        if (a != b) parent[b] = a;
    };

    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> edgeUse;
    for (const ImportedTriangle& triangle : mesh.triangles) {
        const auto& indices = triangle.indices;
        if (indices[0] >= mesh.vertices.size() || indices[1] >= mesh.vertices.size() ||
            indices[2] >= mesh.vertices.size()) continue;
        used[indices[0]] = used[indices[1]] = used[indices[2]] = 1;
        unite(indices[0], indices[1]);
        unite(indices[1], indices[2]);
        unite(indices[2], indices[0]);
        for (std::size_t edge = 0; edge < 3; ++edge) {
            const std::uint32_t a = indices[edge];
            const std::uint32_t b = indices[(edge + 1) % 3];
            if (a == b) continue;
            ++edgeUse[std::minmax(a, b)];
        }
    }
    for (const auto& [edge, count] : edgeUse) {
        (void)edge;
        if (count == 1) ++summary.boundaryEdges;
        else if (count > 2) ++summary.nonManifoldEdges;
    }
    std::set<std::uint32_t> roots;
    for (std::uint32_t index = 0; index < used.size(); ++index) {
        if (used[index] != 0) roots.insert(find_root(index, find_root));
    }
    summary.islands = roots.size();
    return summary;
}

[[nodiscard]] std::vector<WorldTriangle> flatten_triangles(
    const ImportedScene& scene,
    AssetCookerStats& stats,
    std::vector<ImportDiagnostic>& diagnostics) {
    std::vector<WorldTriangle> triangles;
    for (const ImportedNode& node : scene.nodes) {
        if (!node.mesh.has_value()) continue;
        if (*node.mesh >= scene.meshes.size()) throw std::runtime_error("node references invalid mesh");
        const ImportedMesh& mesh = scene.meshes[*node.mesh];
        stats.sourceVertices += mesh.vertices.size();
        stats.sourceTriangles += mesh.triangles.size();
        const MeshTopologySummary topology = analyze_mesh_topology(mesh);
        stats.boundaryEdges += topology.boundaryEdges;
        stats.nonManifoldEdges += topology.nonManifoldEdges;
        stats.meshIslands += topology.islands;
        for (const ImportedTriangle& triangle : mesh.triangles) {
            if (triangle.indices[0] >= mesh.vertices.size() || triangle.indices[1] >= mesh.vertices.size() ||
                triangle.indices[2] >= mesh.vertices.size()) {
                throw std::runtime_error("imported triangle references invalid vertex");
            }
            WorldTriangle world;
            for (std::size_t i = 0; i < 3; ++i) {
                const ImportedVertex& vertex = mesh.vertices[triangle.indices[i]];
                world.vertices[i] = transform_point(node.worldTransform, vertex.position);
                world.texcoords[i] = vertex.texcoord;
                world.colors[i] = vertex.color;
            }
            const Float3 normal = cross(sub(world.vertices[1], world.vertices[0]), sub(world.vertices[2], world.vertices[0]));
            if (length_squared3(normal) <= kDegenerateAreaSquared) {
                ++stats.degenerateTriangles;
                continue;
            }
            world.sourceMaterial = std::min<std::uint32_t>(triangle.materialIndex,
                static_cast<std::uint32_t>(scene.materials.empty() ? 0 : scene.materials.size() - 1U));
            triangles.push_back(world);
        }
    }
    stats.decodedImages = scene.images.size();
    for (const ImportedImage& image : scene.images) stats.decodedImageBytes += image.rgba8.size();
    if (triangles.empty()) {
        diagnostics.push_back({ImportDiagnostic::Severity::Error, "NO_TRIANGLES", "The selected scene contains no usable triangles."});
    }
    return triangles;
}

[[nodiscard]] float wrap_coordinate(float value, ImportedWrapMode mode) noexcept {
    switch (mode) {
    case ImportedWrapMode::ClampToEdge:
        return std::clamp(value, 0.0F, std::nextafter(1.0F, 0.0F));
    case ImportedWrapMode::MirroredRepeat: {
        float integral = std::floor(value);
        float fractional = value - integral;
        const auto period = static_cast<std::int64_t>(integral);
        if ((period & 1LL) != 0) fractional = 1.0F - fractional;
        return std::clamp(fractional, 0.0F, std::nextafter(1.0F, 0.0F));
    }
    case ImportedWrapMode::Repeat:
    default:
        value -= std::floor(value);
        return std::clamp(value, 0.0F, std::nextafter(1.0F, 0.0F));
    }
}

[[nodiscard]] std::int32_t wrap_texel(std::int32_t value, std::int32_t size, ImportedWrapMode mode) noexcept {
    if (size <= 1) return 0;
    if (mode == ImportedWrapMode::ClampToEdge) return std::clamp(value, 0, size - 1);
    if (mode == ImportedWrapMode::Repeat) {
        const std::int32_t remainder = value % size;
        return remainder < 0 ? remainder + size : remainder;
    }
    const std::int32_t period = size * 2;
    std::int32_t remainder = value % period;
    if (remainder < 0) remainder += period;
    return remainder < size ? remainder : period - 1 - remainder;
}

[[nodiscard]] Float4 image_texel_linear(const ImportedImage& image, std::int32_t x, std::int32_t y) noexcept {
    const std::size_t index = (static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x)) * 4U;
    return {
        srgb_to_linear(static_cast<float>(image.rgba8[index]) / 255.0F),
        srgb_to_linear(static_cast<float>(image.rgba8[index + 1U]) / 255.0F),
        srgb_to_linear(static_cast<float>(image.rgba8[index + 2U]) / 255.0F),
        static_cast<float>(image.rgba8[index + 3U]) / 255.0F,
    };
}

[[nodiscard]] Float4 sample_texture_linear(
    const ImportedScene& scene,
    std::uint32_t textureIndex,
    Float2 uv) {
    const ImportedTexture& texture = scene.textures.at(textureIndex);
    const ImportedImage& image = scene.images.at(texture.imageIndex);
    const ImportedSampler defaultSampler{};
    const ImportedSampler& sampler = texture.samplerIndex.has_value() ? scene.samplers.at(*texture.samplerIndex) : defaultSampler;
    if (image.width == 0 || image.height == 0 || image.rgba8.size() !=
        static_cast<std::size_t>(image.width) * image.height * 4U) {
        throw std::runtime_error("decoded texture image is invalid");
    }
    const float u = wrap_coordinate(uv.x, sampler.wrapS);
    const float v = wrap_coordinate(uv.y, sampler.wrapT);
    if (sampler.magFilter == ImportedTextureFilter::Nearest) {
        const std::int32_t x = std::min<std::int32_t>(static_cast<std::int32_t>(u * static_cast<float>(image.width)),
                                                     static_cast<std::int32_t>(image.width) - 1);
        const std::int32_t y = std::min<std::int32_t>(static_cast<std::int32_t>(v * static_cast<float>(image.height)),
                                                     static_cast<std::int32_t>(image.height) - 1);
        return image_texel_linear(image, x, y);
    }
    const float fx = u * static_cast<float>(image.width) - 0.5F;
    const float fy = v * static_cast<float>(image.height) - 0.5F;
    const std::int32_t x0 = static_cast<std::int32_t>(std::floor(fx));
    const std::int32_t y0 = static_cast<std::int32_t>(std::floor(fy));
    const float tx = fx - std::floor(fx);
    const float ty = fy - std::floor(fy);
    const Float4 c00 = image_texel_linear(image, wrap_texel(x0, static_cast<std::int32_t>(image.width), sampler.wrapS),
        wrap_texel(y0, static_cast<std::int32_t>(image.height), sampler.wrapT));
    const Float4 c10 = image_texel_linear(image, wrap_texel(x0 + 1, static_cast<std::int32_t>(image.width), sampler.wrapS),
        wrap_texel(y0, static_cast<std::int32_t>(image.height), sampler.wrapT));
    const Float4 c01 = image_texel_linear(image, wrap_texel(x0, static_cast<std::int32_t>(image.width), sampler.wrapS),
        wrap_texel(y0 + 1, static_cast<std::int32_t>(image.height), sampler.wrapT));
    const Float4 c11 = image_texel_linear(image, wrap_texel(x0 + 1, static_cast<std::int32_t>(image.width), sampler.wrapS),
        wrap_texel(y0 + 1, static_cast<std::int32_t>(image.height), sampler.wrapT));
    const auto lerp = [](Float4 a, Float4 b, float t) {
        return Float4{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                      a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    };
    return lerp(lerp(c00, c10, tx), lerp(c01, c11, tx), ty);
}

[[nodiscard]] std::array<float, 3> barycentric_coordinates(
    Float3 point, Float3 a, Float3 b, Float3 c) noexcept {
    const Float3 v0 = sub(b, a);
    const Float3 v1 = sub(c, a);
    const Float3 v2 = sub(point, a);
    const float d00 = dot3(v0, v0);
    const float d01 = dot3(v0, v1);
    const float d11 = dot3(v1, v1);
    const float d20 = dot3(v2, v0);
    const float d21 = dot3(v2, v1);
    const float denominator = d00 * d11 - d01 * d01;
    if (std::abs(denominator) <= 1.0e-20F) return {1.0F, 0.0F, 0.0F};
    const float v = (d11 * d20 - d01 * d21) / denominator;
    const float w = (d00 * d21 - d01 * d20) / denominator;
    return {1.0F - v - w, v, w};
}

struct SampledSurfaceMaterial {
    std::uint32_t sourceMaterial{};
    std::uint32_t rgba8{};
    bool accepted{true};
};

[[nodiscard]] SampledSurfaceMaterial evaluate_surface_material(
    const ImportedScene& scene,
    const WorldTriangle& triangle,
    const std::array<float, 3>& barycentric,
    AssetCookerStats& stats) {
    const ImportedMaterial& material = scene.materials.at(triangle.sourceMaterial);
    Float2 uv{};
    Float4 vertexColor{};
    vertexColor.w = 0.0F;
    for (std::size_t i = 0; i < 3; ++i) {
        uv.x += triangle.texcoords[i].x * barycentric[i];
        uv.y += triangle.texcoords[i].y * barycentric[i];
        vertexColor.x += triangle.colors[i].x * barycentric[i];
        vertexColor.y += triangle.colors[i].y * barycentric[i];
        vertexColor.z += triangle.colors[i].z * barycentric[i];
        vertexColor.w += triangle.colors[i].w * barycentric[i];
    }
    Float4 color{
        material.baseColorFactor.x * vertexColor.x,
        material.baseColorFactor.y * vertexColor.y,
        material.baseColorFactor.z * vertexColor.z,
        material.baseColorFactor.w * vertexColor.w,
    };
    if (material.baseColorTexture.has_value() && material.baseColorTexcoord == 0U) {
        const Float4 texture = sample_texture_linear(scene, *material.baseColorTexture, uv);
        color.x *= texture.x;
        color.y *= texture.y;
        color.z *= texture.z;
        color.w *= texture.w;
        ++stats.textureSamples;
    }
    color.x = std::clamp(color.x, 0.0F, 1.0F);
    color.y = std::clamp(color.y, 0.0F, 1.0F);
    color.z = std::clamp(color.z, 0.0F, 1.0F);
    color.w = std::clamp(color.w, 0.0F, 1.0F);
    if (material.alphaMode == ImportedAlphaMode::Mask && color.w < material.alphaCutoff) {
        ++stats.alphaRejectedSamples;
        return {triangle.sourceMaterial, 0U, false};
    }
    if (material.alphaMode == ImportedAlphaMode::Opaque) color.w = 1.0F;
    return {triangle.sourceMaterial, pack_rgba8(color), true};
}

void rasterize_surface(
    DenseCookGrid& grid,
    const ImportedScene& scene,
    const std::vector<WorldTriangle>& triangles,
    float voxelSize,
    Float3 modelMinimum,
    Float3 modelMaximum,
    AssetCookerStats& stats) {
    const Float3 halfExtent{0.5F, 0.5F, 0.5F};
    const Float3 modelMinVoxel{modelMinimum.x / voxelSize, modelMinimum.y / voxelSize, modelMinimum.z / voxelSize};
    const Float3 modelMaxVoxel{modelMaximum.x / voxelSize, modelMaximum.y / voxelSize, modelMaximum.z / voxelSize};
    constexpr float kBoundaryEpsilon = 1.0e-5F;
    const auto candidate_axis_bounds = [&](float triangleMinimum, float triangleMaximum,
                                           float objectMinimum, float objectMaximum) {
        std::int32_t minimum = static_cast<std::int32_t>(std::floor(triangleMinimum));
        std::int32_t maximum = static_cast<std::int32_t>(std::ceil(triangleMaximum)) - 1;
        if (std::abs(triangleMaximum - triangleMinimum) <= kBoundaryEpsilon) {
            const float rounded = std::round(triangleMinimum);
            if (std::abs(triangleMinimum - rounded) <= kBoundaryEpsilon) {
                const std::int32_t boundary = static_cast<std::int32_t>(rounded);
                if (objectMaximum - objectMinimum > kBoundaryEpsilon &&
                    std::abs(triangleMinimum - objectMaximum) <= kBoundaryEpsilon) {
                    minimum = boundary - 1;
                    maximum = boundary - 1;
                } else {
                    minimum = boundary;
                    maximum = boundary;
                }
            }
        }
        if (maximum < minimum) maximum = minimum;
        return std::pair{minimum, maximum};
    };
    for (const WorldTriangle& sourceTriangle : triangles) {
        std::array<Float3, 3> triangle;
        for (std::size_t i = 0; i < 3; ++i) {
            triangle[i] = {sourceTriangle.vertices[i].x / voxelSize,
                           sourceTriangle.vertices[i].y / voxelSize,
                           sourceTriangle.vertices[i].z / voxelSize};
        }
        const float minX = std::min({triangle[0].x, triangle[1].x, triangle[2].x});
        const float minY = std::min({triangle[0].y, triangle[1].y, triangle[2].y});
        const float minZ = std::min({triangle[0].z, triangle[1].z, triangle[2].z});
        const float maxX = std::max({triangle[0].x, triangle[1].x, triangle[2].x});
        const float maxY = std::max({triangle[0].y, triangle[1].y, triangle[2].y});
        const float maxZ = std::max({triangle[0].z, triangle[1].z, triangle[2].z});
        const auto [candidateMinX, candidateMaxX] = candidate_axis_bounds(minX, maxX, modelMinVoxel.x, modelMaxVoxel.x);
        const auto [candidateMinY, candidateMaxY] = candidate_axis_bounds(minY, maxY, modelMinVoxel.y, modelMaxVoxel.y);
        const auto [candidateMinZ, candidateMaxZ] = candidate_axis_bounds(minZ, maxZ, modelMinVoxel.z, modelMaxVoxel.z);
        Int3 minimum{std::max(grid.minimum().x, candidateMinX), std::max(grid.minimum().y, candidateMinY),
                     std::max(grid.minimum().z, candidateMinZ)};
        Int3 maximum{std::min(grid.maximum().x, std::max(minimum.x, candidateMaxX)),
                     std::min(grid.maximum().y, std::max(minimum.y, candidateMaxY)),
                     std::min(grid.maximum().z, std::max(minimum.z, candidateMaxZ))};
        for (std::int32_t z = minimum.z; z <= maximum.z; ++z) {
            for (std::int32_t y = minimum.y; y <= maximum.y; ++y) {
                for (std::int32_t x = minimum.x; x <= maximum.x; ++x) {
                    const Float3 center{static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F,
                                        static_cast<float>(z) + 0.5F};
                    if (!triangle_intersects_box(triangle, center, halfExtent)) continue;
                    const Float3 closest = closest_point_on_triangle(center, triangle[0], triangle[1], triangle[2]);
                    const float distanceSquared = length_squared3(sub(center, closest));
                    const auto barycentric = barycentric_coordinates(closest, triangle[0], triangle[1], triangle[2]);
                    const SampledSurfaceMaterial sampled = evaluate_surface_material(scene, sourceTriangle, barycentric, stats);
                    SurfaceCell& cell = grid.cell({x, y, z});
                    const std::uint64_t sampledKey = sampled.accepted
                        ? material_sample_key(sampled.sourceMaterial, sampled.rgba8)
                        : std::numeric_limits<std::uint64_t>::max();
                    const std::uint64_t currentKey = cell.occupied() ? cell.key() : std::numeric_limits<std::uint64_t>::max();
                    if (!cell.candidateSeen || distanceSquared < cell.distanceSquared ||
                        (distanceSquared == cell.distanceSquared && sampledKey < currentKey)) {
                        cell.candidateSeen = true;
                        cell.distanceSquared = distanceSquared;
                        cell.surface = sampled.accepted;
                        cell.sourceMaterial = sampled.accepted ? sampled.sourceMaterial : kNoSourceMaterial;
                        cell.rgba8 = sampled.accepted ? sampled.rgba8 : 0U;
                    }
                }
            }
        }
    }
}

void fill_interior(DenseCookGrid& grid, std::uint32_t sourceMaterial, std::uint32_t rgba8, AssetCookerStats& stats) {
    std::vector<std::uint8_t> exterior(grid.count(), 0);
    std::deque<std::size_t> queue;
    const Int3 minimum = grid.minimum();
    const Int3 maximum = grid.maximum();
    auto enqueue = [&](Int3 coordinate) {
        const std::size_t index = grid.index(coordinate);
        if (exterior[index] != 0 || grid.cell(index).surface) return;
        exterior[index] = 1;
        queue.push_back(index);
    };
    for (std::int32_t z = minimum.z; z <= maximum.z; ++z) for (std::int32_t y = minimum.y; y <= maximum.y; ++y) {
        enqueue({minimum.x, y, z}); enqueue({maximum.x, y, z});
    }
    for (std::int32_t z = minimum.z; z <= maximum.z; ++z) for (std::int32_t x = minimum.x; x <= maximum.x; ++x) {
        enqueue({x, minimum.y, z}); enqueue({x, maximum.y, z});
    }
    for (std::int32_t y = minimum.y; y <= maximum.y; ++y) for (std::int32_t x = minimum.x; x <= maximum.x; ++x) {
        enqueue({x, y, minimum.z}); enqueue({x, y, maximum.z});
    }
    constexpr std::array<Int3, 6> neighbors{{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
    while (!queue.empty()) {
        const std::size_t currentIndex = queue.front(); queue.pop_front();
        const Int3 current = grid.coordinate(currentIndex);
        for (Int3 offset : neighbors) {
            const Int3 next{current.x + offset.x, current.y + offset.y, current.z + offset.z};
            if (!grid.contains(next)) continue;
            const std::size_t nextIndex = grid.index(next);
            if (exterior[nextIndex] != 0 || grid.cell(nextIndex).surface) continue;
            exterior[nextIndex] = 1;
            queue.push_back(nextIndex);
        }
    }
    for (std::size_t index = 0; index < grid.count(); ++index) {
        SurfaceCell& cell = grid.cell(index);
        if (!cell.surface && exterior[index] == 0) {
            cell.sourceMaterial = sourceMaterial;
            cell.rgba8 = rgba8;
            ++stats.interiorVoxels;
        }
    }
}

void thicken_shell(DenseCookGrid& grid, std::uint32_t radius) {
    if (radius <= 1) return;
    constexpr std::array<Int3, 6> neighbors{{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
    std::vector<SurfaceCell> current(grid.count());
    for (std::size_t i = 0; i < grid.count(); ++i) current[i] = grid.cell(i);
    for (std::uint32_t step = 1; step < radius; ++step) {
        std::vector<SurfaceCell> next = current;
        for (std::size_t i = 0; i < current.size(); ++i) {
            if (!current[i].occupied()) continue;
            const Int3 coordinate = grid.coordinate(i);
            for (Int3 offset : neighbors) {
                const Int3 neighbor{coordinate.x + offset.x, coordinate.y + offset.y, coordinate.z + offset.z};
                if (!grid.contains(neighbor)) continue;
                SurfaceCell& destination = next[grid.index(neighbor)];
                if (!destination.occupied()) {
                    destination = current[i];
                    destination.surface = false;
                    destination.distanceSquared = std::numeric_limits<float>::infinity();
                }
            }
        }
        current.swap(next);
    }
    for (std::size_t i = 0; i < current.size(); ++i) grid.cell(i) = current[i];
}

[[nodiscard]] DenseCookGrid downsample_grid(
    const DenseCookGrid& high,
    std::uint32_t factor,
    float coverageThreshold,
    bool preserveThin,
    std::uint64_t maximumWorkingVoxels) {
    if (factor == 1) return high;
    const Int3 highMin = high.minimum();
    const Int3 highMax = high.maximum();
    const Int3 lowMin{floor_div(highMin.x, static_cast<std::int32_t>(factor)),
        floor_div(highMin.y, static_cast<std::int32_t>(factor)), floor_div(highMin.z, static_cast<std::int32_t>(factor))};
    const Int3 lowMax{floor_div(highMax.x, static_cast<std::int32_t>(factor)),
        floor_div(highMax.y, static_cast<std::int32_t>(factor)), floor_div(highMax.z, static_cast<std::int32_t>(factor))};
    DenseCookGrid low(lowMin, lowMax, maximumWorkingVoxels);
    const std::uint32_t samplesPerVoxel = factor * factor * factor;
    const std::uint32_t required = std::max<std::uint32_t>(1,
        static_cast<std::uint32_t>(std::ceil(coverageThreshold * static_cast<float>(samplesPerVoxel))));
    for (std::int32_t z = lowMin.z; z <= lowMax.z; ++z) for (std::int32_t y = lowMin.y; y <= lowMax.y; ++y)
        for (std::int32_t x = lowMin.x; x <= lowMax.x; ++x) {
            std::vector<std::pair<std::uint64_t, std::uint32_t>> counts;
            std::uint32_t occupied = 0;
            bool surface = false;
            for (std::uint32_t dz = 0; dz < factor; ++dz) for (std::uint32_t dy = 0; dy < factor; ++dy)
                for (std::uint32_t dx = 0; dx < factor; ++dx) {
                    const Int3 coordinate{x * static_cast<std::int32_t>(factor) + static_cast<std::int32_t>(dx),
                        y * static_cast<std::int32_t>(factor) + static_cast<std::int32_t>(dy),
                        z * static_cast<std::int32_t>(factor) + static_cast<std::int32_t>(dz)};
                    if (!high.contains(coordinate)) continue;
                    const SurfaceCell& sample = high.cell(coordinate);
                    if (!sample.occupied()) continue;
                    ++occupied;
                    surface = surface || sample.surface;
                    const std::uint64_t key = sample.key();
                    auto it = std::lower_bound(counts.begin(), counts.end(), key,
                        [](const auto& entry, std::uint64_t value) { return entry.first < value; });
                    if (it == counts.end() || it->first != key) counts.insert(it, {key, 1U});
                    else ++it->second;
                }
            if (occupied < required && !(preserveThin && surface)) continue;
            std::uint64_t bestKey = 0;
            std::uint32_t bestCount = 0;
            for (const auto& [key, count] : counts) {
                if (count > bestCount || (count == bestCount && key < bestKey)) { bestKey = key; bestCount = count; }
            }
            SurfaceCell& destination = low.cell({x, y, z});
            destination.sourceMaterial = static_cast<std::uint32_t>(bestKey >> 32U);
            destination.rgba8 = static_cast<std::uint32_t>(bestKey);
            destination.surface = surface;
        }
    return low;
}

[[nodiscard]] VoxelMaterialDefinition base_material_definition(const ImportedMaterial& imported) {
    VoxelMaterialDefinition material;
    material.name = imported.name;
    material.baseColor = imported.baseColorFactor;
    material.emissive = imported.emissiveFactor;
    material.metallic = std::clamp(imported.metallicFactor, 0.0F, 1.0F);
    material.roughness = std::clamp(imported.roughnessFactor, 0.0F, 1.0F);
    material.transparent = imported.alphaMode == ImportedAlphaMode::Blend || imported.baseColorFactor.w < 0.999F;
    material.densityKilogramsPerCubicMeter = material.metallic > 0.5F ? 7800.0F : 1000.0F;
    material.structuralStrength = material.metallic > 0.5F ? 4.0F : 1.0F;
    material.fractureResistance = material.metallic > 0.5F ? 3.0F : 1.0F;
    return material;
}

struct PaletteColorPoint {
    std::uint32_t rgba8{};
    std::uint64_t count{};
};

[[nodiscard]] std::vector<std::uint32_t> median_cut_palette(
    std::vector<PaletteColorPoint> points,
    std::uint32_t budget) {
    if (points.empty() || budget == 0) return {};
    std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.rgba8 < b.rgba8; });
    if (points.size() <= budget) {
        std::vector<std::uint32_t> colors;
        for (const auto& point : points) colors.push_back(point.rgba8);
        return colors;
    }
    struct Bucket { std::vector<PaletteColorPoint> points; };
    std::vector<Bucket> buckets;
    buckets.push_back({std::move(points)});
    const auto channel = [](std::uint32_t color, int c) { return static_cast<std::uint8_t>((color >> (8U * c)) & 0xFFU); };
    while (buckets.size() < budget) {
        std::size_t splitIndex = buckets.size();
        std::uint64_t bestScore = 0;
        int bestChannel = 0;
        for (std::size_t i = 0; i < buckets.size(); ++i) {
            if (buckets[i].points.size() < 2) continue;
            std::array<int, 4> minimum{255,255,255,255};
            std::array<int, 4> maximum{0,0,0,0};
            std::uint64_t weight = 0;
            for (const auto& point : buckets[i].points) {
                weight += point.count;
                for (int c = 0; c < 4; ++c) {
                    const int value = channel(point.rgba8, c);
                    minimum[c] = std::min(minimum[c], value);
                    maximum[c] = std::max(maximum[c], value);
                }
            }
            int selectedChannel = 0;
            int range = maximum[0] - minimum[0];
            for (int c = 1; c < 4; ++c) {
                const int candidate = maximum[c] - minimum[c];
                if (candidate > range) { range = candidate; selectedChannel = c; }
            }
            const std::uint64_t score = static_cast<std::uint64_t>(range + 1) * weight;
            if (score > bestScore) { bestScore = score; splitIndex = i; bestChannel = selectedChannel; }
        }
        if (splitIndex == buckets.size()) break;
        auto pointsToSplit = std::move(buckets[splitIndex].points);
        std::stable_sort(pointsToSplit.begin(), pointsToSplit.end(), [&](const auto& a, const auto& b) {
            const auto ca = channel(a.rgba8, bestChannel);
            const auto cb = channel(b.rgba8, bestChannel);
            return ca != cb ? ca < cb : a.rgba8 < b.rgba8;
        });
        std::uint64_t total = 0;
        for (const auto& point : pointsToSplit) total += point.count;
        std::uint64_t prefix = 0;
        std::size_t split = 1;
        for (; split < pointsToSplit.size(); ++split) {
            prefix += pointsToSplit[split - 1].count;
            if (prefix * 2ULL >= total) break;
        }
        Bucket left, right;
        left.points.assign(pointsToSplit.begin(), pointsToSplit.begin() + static_cast<std::ptrdiff_t>(split));
        right.points.assign(pointsToSplit.begin() + static_cast<std::ptrdiff_t>(split), pointsToSplit.end());
        buckets[splitIndex] = std::move(left);
        buckets.push_back(std::move(right));
    }
    std::vector<std::uint32_t> representatives;
    representatives.reserve(buckets.size());
    for (const Bucket& bucket : buckets) {
        std::array<std::uint64_t, 4> sum{};
        std::uint64_t weight = 0;
        for (const auto& point : bucket.points) {
            weight += point.count;
            for (int c = 0; c < 4; ++c) sum[c] += static_cast<std::uint64_t>((point.rgba8 >> (8U * c)) & 0xFFU) * point.count;
        }
        std::uint32_t color = 0;
        for (int c = 0; c < 4; ++c) color |= static_cast<std::uint32_t>((sum[c] + weight / 2ULL) / weight) << (8U * c);
        representatives.push_back(color);
    }
    std::sort(representatives.begin(), representatives.end());
    representatives.erase(std::unique(representatives.begin(), representatives.end()), representatives.end());
    return representatives;
}

[[nodiscard]] std::uint64_t color_distance(std::uint32_t a, std::uint32_t b) noexcept {
    std::uint64_t distance = 0;
    for (int c = 0; c < 4; ++c) {
        const std::int32_t delta = static_cast<std::int32_t>((a >> (8U * c)) & 0xFFU) -
            static_cast<std::int32_t>((b >> (8U * c)) & 0xFFU);
        const std::uint64_t weight = c == 3 ? 2ULL : 1ULL;
        distance += weight * static_cast<std::uint64_t>(delta * delta);
    }
    return distance;
}

struct BuiltPalette {
    std::vector<VoxelMaterialDefinition> materials;
    std::map<std::uint64_t, MaterialId> mapping;
};

[[nodiscard]] BuiltPalette build_material_palette(
    const ImportedScene& scene,
    const DenseCookGrid& grid,
    std::uint32_t maximumMaterials,
    AssetCookerStats& stats,
    std::vector<ImportDiagnostic>& diagnostics) {
    if (maximumMaterials < 2 || maximumMaterials > 256) throw std::runtime_error("maximumPaletteMaterials must be in [2, 256]");
    std::map<std::uint32_t, std::map<std::uint32_t, std::uint64_t>> grouped;
    for (std::size_t i = 0; i < grid.count(); ++i) {
        const SurfaceCell& cell = grid.cell(i);
        if (cell.occupied()) ++grouped[cell.sourceMaterial][cell.rgba8];
    }
    std::uint64_t unique = 0;
    for (const auto& [source, colors] : grouped) { (void)source; unique += colors.size(); }
    stats.uniqueMaterialSamples = unique;
    const std::uint32_t available = maximumMaterials - 1U;
    if (grouped.size() > available) throw std::runtime_error("used source material count exceeds palette budget");

    std::map<std::uint32_t, std::uint32_t> budgets;
    for (const auto& [source, colors] : grouped) { (void)colors; budgets[source] = 1U; }
    std::uint32_t remaining = available - static_cast<std::uint32_t>(grouped.size());
    while (remaining != 0) {
        std::optional<std::uint32_t> selected;
        double bestNeed = -1.0;
        for (const auto& [source, colors] : grouped) {
            if (budgets[source] >= colors.size()) continue;
            std::uint64_t weight = 0;
            for (const auto& [color, count] : colors) { (void)color; weight += count; }
            const double need = static_cast<double>(weight) / static_cast<double>(budgets[source]);
            if (need > bestNeed || (need == bestNeed && (!selected.has_value() || source < *selected))) {
                selected = source; bestNeed = need;
            }
        }
        if (!selected.has_value()) break;
        ++budgets[*selected];
        --remaining;
    }

    BuiltPalette output;
    VoxelMaterialDefinition air;
    air.name = "Air";
    air.baseColor = {0.0F, 0.0F, 0.0F, 0.0F};
    air.densityKilogramsPerCubicMeter = 0.0F;
    air.structural = false;
    output.materials.push_back(air);
    for (const auto& [source, colors] : grouped) {
        std::vector<PaletteColorPoint> points;
        for (const auto& [color, count] : colors) points.push_back({color, count});
        const std::vector<std::uint32_t> representatives = median_cut_palette(points, budgets[source]);
        std::vector<std::pair<std::uint32_t, MaterialId>> representativeIds;
        for (std::uint32_t color : representatives) {
            if (output.materials.size() >= 256) throw std::runtime_error("material palette exceeded MaterialId range");
            VoxelMaterialDefinition material = base_material_definition(scene.materials.at(source));
            material.baseColor = unpack_rgba8(color);
            material.transparent = material.transparent || material.baseColor.w < 0.999F;
            std::ostringstream suffix;
            suffix << '#' << std::hex << std::setw(8) << std::setfill('0') << color;
            material.name += suffix.str();
            const MaterialId id = static_cast<MaterialId>(output.materials.size());
            output.materials.push_back(std::move(material));
            representativeIds.push_back({color, id});
        }
        for (const auto& [color, count] : colors) {
            (void)count;
            std::uint64_t bestDistance = std::numeric_limits<std::uint64_t>::max();
            MaterialId best = representativeIds.front().second;
            for (const auto& [representative, id] : representativeIds) {
                const std::uint64_t distance = color_distance(color, representative);
                if (distance < bestDistance || (distance == bestDistance && id < best)) {
                    bestDistance = distance; best = id;
                }
            }
            output.mapping.emplace(material_sample_key(source, color), best);
        }
    }
    stats.paletteMaterials = output.materials.size();
    if (unique + 1ULL > maximumMaterials) {
        diagnostics.push_back({ImportDiagnostic::Severity::Info, "PALETTE_QUANTIZED",
            std::to_string(unique) + " texture/material samples were deterministically reduced to " +
            std::to_string(output.materials.size() - 1U) + " occupied material entries."});
    }
    return output;
}

[[nodiscard]] std::string html_escape(std::string_view text) {
    std::string output;
    output.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&': output += "&amp;"; break;
        case '<': output += "&lt;"; break;
        case '>': output += "&gt;"; break;
        case '"': output += "&quot;"; break;
        case '\'': output += "&#39;"; break;
        default: output.push_back(c); break;
        }
    }
    return output;
}

} // namespace

ImportedModelResult import_model(const std::filesystem::path& sourcePath, const ModelImportOptions& options) {
    const std::string extension = sourcePath.extension().string();
    if (extension == ".gltf" || extension == ".GLTF" || extension == ".glb" || extension == ".GLB") {
        return import_gltf(sourcePath, options);
    }
    if (extension == ".obj" || extension == ".OBJ") return import_obj(sourcePath, options);
    ImportedModelResult result;
    result.diagnostics.push_back({ImportDiagnostic::Severity::Error, "UNSUPPORTED_FORMAT",
        "Unsupported model extension: " + extension + ". Supported formats are .gltf, .glb, and .obj."});
    return result;
}

CookedVoxelAsset voxelize_scene(const ImportedScene& scene, const VoxelizeSettings& settings) {
    CookedVoxelAsset output(settings.objectId);
    output.voxelSizeMeters = settings.voxelSizeMeters;
    if (!(settings.voxelSizeMeters > 0.0F) || !std::isfinite(settings.voxelSizeMeters)) {
        output.diagnostics.push_back({ImportDiagnostic::Severity::Error, "VOXEL_SIZE", "voxelSizeMeters must be finite and positive."});
        return output;
    }
    if (settings.supersampleFactor != 1 && settings.supersampleFactor != 2 && settings.supersampleFactor != 4) {
        output.diagnostics.push_back({ImportDiagnostic::Severity::Error, "SUPERSAMPLE", "supersampleFactor must be 1, 2, or 4."});
        return output;
    }
    if (settings.maximumPaletteMaterials < 2 || settings.maximumPaletteMaterials > 256) {
        output.diagnostics.push_back({ImportDiagnostic::Severity::Error, "PALETTE_LIMIT", "maximumPaletteMaterials must be in [2, 256]."});
        return output;
    }
    try {
        std::vector<WorldTriangle> triangles = flatten_triangles(scene, output.stats, output.diagnostics);
        if (triangles.empty()) return output;
        if (output.stats.boundaryEdges != 0) {
            output.diagnostics.push_back({
                settings.mode == VoxelizationMode::Solid ? ImportDiagnostic::Severity::Warning
                                                         : ImportDiagnostic::Severity::Info,
                "BOUNDARY_EDGES",
                std::to_string(output.stats.boundaryEdges) +
                    " source-mesh boundary edges were detected. Solid flood filling may leak through open geometry."});
        }
        if (output.stats.nonManifoldEdges != 0) {
            output.diagnostics.push_back({ImportDiagnostic::Severity::Warning, "NON_MANIFOLD_EDGES",
                std::to_string(output.stats.nonManifoldEdges) +
                    " source-mesh edges are shared by more than two triangles."});
        }
        if (output.stats.meshIslands > scene.nodes.size()) {
            output.diagnostics.push_back({ImportDiagnostic::Severity::Info, "MESH_ISLANDS",
                std::to_string(output.stats.meshIslands) +
                    " disconnected mesh islands were found across instantiated meshes."});
        }
        Float3 minimum = triangles.front().vertices.front();
        Float3 maximum = minimum;
        for (const WorldTriangle& triangle : triangles) {
            for (Float3 vertex : triangle.vertices) {
                minimum.x = std::min(minimum.x, vertex.x);
                minimum.y = std::min(minimum.y, vertex.y);
                minimum.z = std::min(minimum.z, vertex.z);
                maximum.x = std::max(maximum.x, vertex.x);
                maximum.y = std::max(maximum.y, vertex.y);
                maximum.z = std::max(maximum.z, vertex.z);
            }
        }
        const float workingVoxelSize = settings.voxelSizeMeters / static_cast<float>(settings.supersampleFactor);
        Int3 workingMinimum{
            static_cast<std::int32_t>(std::floor(minimum.x / workingVoxelSize)) - 1,
            static_cast<std::int32_t>(std::floor(minimum.y / workingVoxelSize)) - 1,
            static_cast<std::int32_t>(std::floor(minimum.z / workingVoxelSize)) - 1,
        };
        Int3 workingMaximum{
            static_cast<std::int32_t>(std::floor(maximum.x / workingVoxelSize)) + 1,
            static_cast<std::int32_t>(std::floor(maximum.y / workingVoxelSize)) + 1,
            static_cast<std::int32_t>(std::floor(maximum.z / workingVoxelSize)) + 1,
        };
        DenseCookGrid working(workingMinimum, workingMaximum, settings.maximumWorkingVoxels);
        rasterize_surface(working, scene, triangles, workingVoxelSize, minimum, maximum, output.stats);
        for (std::size_t i = 0; i < working.count(); ++i) if (working.cell(i).surface) ++output.stats.surfaceVoxels;

        std::uint32_t interiorSource = 0;
        if (settings.interiorMaterial != kAirMaterial) interiorSource = static_cast<std::uint32_t>(settings.interiorMaterial - 1U);
        interiorSource = std::min<std::uint32_t>(interiorSource,
            static_cast<std::uint32_t>(scene.materials.empty() ? 0 : scene.materials.size() - 1U));
        Float4 interiorColor = scene.materials.at(interiorSource).baseColorFactor;
        if (scene.materials.at(interiorSource).alphaMode == ImportedAlphaMode::Opaque) interiorColor.w = 1.0F;
        if (settings.mode == VoxelizationMode::Solid) {
            fill_interior(working, interiorSource, pack_rgba8(interiorColor), output.stats);
        } else if (settings.mode == VoxelizationMode::Shell) {
            thicken_shell(working, settings.shellThicknessVoxels * settings.supersampleFactor);
        }

        DenseCookGrid finalGrid = downsample_grid(working, settings.supersampleFactor,
            settings.downsampleCoverageThreshold, settings.preserveThinSurface, settings.maximumWorkingVoxels);
        BuiltPalette palette = build_material_palette(scene, finalGrid, settings.maximumPaletteMaterials,
            output.stats, output.diagnostics);
        output.materials = std::move(palette.materials);
        bool any = false;
        Int3 outputMinimum = kInt3Max;
        Int3 outputMaximum = kInt3Min;
        for (std::size_t i = 0; i < finalGrid.count(); ++i) {
            const SurfaceCell& cell = finalGrid.cell(i);
            if (!cell.occupied()) continue;
            const MaterialId material = palette.mapping.at(cell.key());
            const Int3 coordinate = finalGrid.coordinate(i);
            output.object.set_voxel(coordinate, material);
            outputMinimum = min_components(outputMinimum, coordinate);
            outputMaximum = max_components(outputMaximum, coordinate);
            ++output.stats.outputVoxels;
            any = true;
        }
        output.stats.outputBricks = output.object.brick_count();
        output.stats.minimumVoxel = any ? outputMinimum : Int3{};
        output.stats.maximumVoxel = any ? outputMaximum : Int3{};
        if (!output.object.validate()) throw std::runtime_error("packed VoxelObject failed validation");
        if (!any) output.diagnostics.push_back({ImportDiagnostic::Severity::Warning, "EMPTY_OUTPUT",
            "Voxelization produced no occupied voxels at the selected resolution."});
        if (output.stats.degenerateTriangles != 0) {
            output.diagnostics.push_back({ImportDiagnostic::Severity::Warning, "DEGENERATE_TRIANGLES",
                std::to_string(output.stats.degenerateTriangles) + " degenerate triangles were removed."});
        }
    } catch (const std::exception& exception) {
        output.diagnostics.push_back({ImportDiagnostic::Severity::Error, "VOXELIZE", exception.what()});
    }
    return output;
}

CookedVoxelAsset cook_model(
    const std::filesystem::path& sourcePath,
    const ModelImportOptions& importOptions,
    const VoxelizeSettings& voxelizeSettings) {
    ImportedModelResult imported = import_model(sourcePath, importOptions);
    if (!imported.success) {
        CookedVoxelAsset failed(voxelizeSettings.objectId);
        failed.voxelSizeMeters = voxelizeSettings.voxelSizeMeters;
        failed.diagnostics = std::move(imported.diagnostics);
        return failed;
    }
    CookedVoxelAsset asset = voxelize_scene(imported.scene, voxelizeSettings);
    asset.diagnostics.insert(asset.diagnostics.begin(), imported.diagnostics.begin(), imported.diagnostics.end());
    return asset;
}


[[nodiscard]] std::uint64_t stable_string_hash(std::string_view text) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char value : text) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1 : hash;
}

void apply_node_settings_object(const JsonValue& object, NodeCookSettings& settings) {
    if (!object.is_object()) throw std::runtime_error("node DVE settings must be an object");
    if (const JsonValue* ignore = object.find("ignore")) settings.ignore = json_bool(ignore);
    if (const JsonValue* mode = object.find("mode")) {
        const std::string text = json_string(mode);
        if (text == "solid") settings.mode = VoxelizationMode::Solid;
        else if (text == "shell") settings.mode = VoxelizationMode::Shell;
        else if (text == "surface") settings.mode = VoxelizationMode::SurfaceOnly;
        else if (text == "ignore") settings.ignore = true;
        else throw std::runtime_error("node mode must be solid, shell, surface, or ignore");
    }
    if (const JsonValue* value = object.find("voxelSizeMeters")) settings.voxelSizeMeters = json_float(value);
    if (const JsonValue* value = object.find("shellThicknessVoxels")) settings.shellThicknessVoxels = json_u32(value);
    if (const JsonValue* value = object.find("interiorMaterial")) {
        const std::uint32_t material = json_u32(value);
        if (material > std::numeric_limits<MaterialId>::max()) throw std::runtime_error("node interiorMaterial exceeds MaterialId range");
        settings.interiorMaterial = static_cast<MaterialId>(material);
    }
    if (const JsonValue* value = object.find("objectId")) settings.objectId = json_u64(value);
    if (const JsonValue* value = object.find("anchored")) settings.anchored = json_bool(value);
    if (const JsonValue* value = object.find("structural")) settings.structural = json_bool(value);
    if (const JsonValue* value = object.find("generateCollision")) settings.generateCollision = json_bool(value);
}

[[nodiscard]] NodeCookSettings node_extras_settings(const ImportedNode& node) {
    NodeCookSettings settings;
    if (node.extrasJson.empty()) return settings;
    const JsonValue root = JsonParser(node.extrasJson).parse();
    if (!root.is_object()) return settings;
    if (const JsonValue* dve = root.find("dve")) apply_node_settings_object(*dve, settings);
    return settings;
}

void merge_node_settings(NodeCookSettings& destination, const NodeCookSettings& source) {
    destination.ignore = destination.ignore || source.ignore;
    if (source.mode.has_value()) destination.mode = source.mode;
    if (source.voxelSizeMeters.has_value()) destination.voxelSizeMeters = source.voxelSizeMeters;
    if (source.shellThicknessVoxels.has_value()) destination.shellThicknessVoxels = source.shellThicknessVoxels;
    if (source.interiorMaterial.has_value()) destination.interiorMaterial = source.interiorMaterial;
    if (source.objectId.has_value()) destination.objectId = source.objectId;
    if (source.anchored.has_value()) destination.anchored = source.anchored;
    if (source.structural.has_value()) destination.structural = source.structural;
    if (source.generateCollision.has_value()) destination.generateCollision = source.generateCollision;
}


struct RigidScaleDecomposition {
    Matrix4 rigid{Matrix4::identity()};
    Float3 scale{1.0F, 1.0F, 1.0F};
    bool valid{};
};

[[nodiscard]] RigidScaleDecomposition decompose_rigid_scale(const Matrix4& matrix) noexcept {
    RigidScaleDecomposition result;
    constexpr float affineTolerance = 1.0e-5F;
    constexpr float orthogonalTolerance = 2.0e-4F;
    if (std::abs(matrix.values[3]) > affineTolerance || std::abs(matrix.values[7]) > affineTolerance ||
        std::abs(matrix.values[11]) > affineTolerance || std::abs(matrix.values[15] - 1.0F) > affineTolerance) {
        return result;
    }
    Float3 columns[3] = {
        {matrix.values[0], matrix.values[1], matrix.values[2]},
        {matrix.values[4], matrix.values[5], matrix.values[6]},
        {matrix.values[8], matrix.values[9], matrix.values[10]},
    };
    float lengths[3] = {
        std::sqrt(length_squared3(columns[0])),
        std::sqrt(length_squared3(columns[1])),
        std::sqrt(length_squared3(columns[2])),
    };
    if (!(lengths[0] > affineTolerance && lengths[1] > affineTolerance && lengths[2] > affineTolerance) ||
        !std::isfinite(lengths[0]) || !std::isfinite(lengths[1]) || !std::isfinite(lengths[2])) {
        return result;
    }
    Float3 axes[3] = {
        mul(columns[0], 1.0F / lengths[0]),
        mul(columns[1], 1.0F / lengths[1]),
        mul(columns[2], 1.0F / lengths[2]),
    };
    if (std::abs(dot3(axes[0], axes[1])) > orthogonalTolerance ||
        std::abs(dot3(axes[0], axes[2])) > orthogonalTolerance ||
        std::abs(dot3(axes[1], axes[2])) > orthogonalTolerance) {
        return result;
    }
    float determinant = dot3(cross(axes[0], axes[1]), axes[2]);
    if (!std::isfinite(determinant) || std::abs(std::abs(determinant) - 1.0F) > 5.0e-4F) return result;
    if (determinant < 0.0F) {
        lengths[2] = -lengths[2];
        axes[2] = mul(axes[2], -1.0F);
    }
    result.rigid = Matrix4::identity();
    result.rigid.values[0] = axes[0].x;
    result.rigid.values[1] = axes[0].y;
    result.rigid.values[2] = axes[0].z;
    result.rigid.values[4] = axes[1].x;
    result.rigid.values[5] = axes[1].y;
    result.rigid.values[6] = axes[1].z;
    result.rigid.values[8] = axes[2].x;
    result.rigid.values[9] = axes[2].y;
    result.rigid.values[10] = axes[2].z;
    result.rigid.values[12] = matrix.values[12];
    result.rigid.values[13] = matrix.values[13];
    result.rigid.values[14] = matrix.values[14];
    result.scale = {lengths[0], lengths[1], lengths[2]};
    result.valid = true;
    return result;
}

void bake_scale_into_mesh(ImportedMesh& mesh, Float3 scale) noexcept {
    for (ImportedVertex& vertex : mesh.vertices) {
        vertex.position.x *= scale.x;
        vertex.position.y *= scale.y;
        vertex.position.z *= scale.z;
        Float3 normal{
            scale.x != 0.0F ? vertex.normal.x / scale.x : 0.0F,
            scale.y != 0.0F ? vertex.normal.y / scale.y : 0.0F,
            scale.z != 0.0F ? vertex.normal.z / scale.z : 0.0F,
        };
        const float lengthSquared = length_squared3(normal);
        if (lengthSquared > 1.0e-20F) vertex.normal = mul(normal, 1.0F / std::sqrt(lengthSquared));
    }
}

[[nodiscard]] std::string node_path(const ImportedScene& scene, std::uint32_t index) {
    std::vector<std::string_view> elements;
    std::optional<std::uint32_t> current = index;
    while (current.has_value()) {
        elements.push_back(scene.nodes.at(*current).name);
        current = scene.nodes.at(*current).parent;
    }
    std::string path;
    for (auto it = elements.rbegin(); it != elements.rend(); ++it) {
        path.push_back('/');
        path.append(*it);
    }
    return path.empty() ? "/" : path;
}

CookedVoxelScene voxelize_scene_objects(const ImportedScene& scene, const SceneCookSettings& settings) {
    CookedVoxelScene output;
    output.name = scene.name;
    if (!settings.splitByNode) {
        CookedVoxelObject object(settings.defaults.objectId);
        object.name = scene.name;
        object.nodePath = "/";
        object.asset = voxelize_scene(scene, settings.defaults);
        output.diagnostics = object.asset.diagnostics;
        bool hasError = false;
        for (const auto& diagnostic : output.diagnostics) hasError = hasError || diagnostic.severity == ImportDiagnostic::Severity::Error;
        output.objects.push_back(std::move(object));
        output.success = !hasError;
        return output;
    }

    std::vector<std::optional<std::uint32_t>> nodeToObject(scene.nodes.size());
    for (std::uint32_t nodeIndex = 0; nodeIndex < scene.nodes.size(); ++nodeIndex) {
        const ImportedNode& node = scene.nodes[nodeIndex];
        if (!node.mesh.has_value()) continue;
        NodeCookSettings nodeSettings = node_extras_settings(node);
        if (const auto it = settings.nodeOverrides.find(node.name); it != settings.nodeOverrides.end()) {
            merge_node_settings(nodeSettings, it->second);
        }
        if (nodeSettings.ignore) continue;

        VoxelizeSettings voxel = settings.defaults;
        if (nodeSettings.mode.has_value()) voxel.mode = *nodeSettings.mode;
        if (nodeSettings.voxelSizeMeters.has_value()) voxel.voxelSizeMeters = *nodeSettings.voxelSizeMeters;
        if (nodeSettings.shellThicknessVoxels.has_value()) voxel.shellThicknessVoxels = *nodeSettings.shellThicknessVoxels;
        if (nodeSettings.interiorMaterial.has_value()) voxel.interiorMaterial = *nodeSettings.interiorMaterial;
        const std::string path = node_path(scene, nodeIndex);
        voxel.objectId = nodeSettings.objectId.value_or(stable_string_hash(scene.name + path));

        ImportedScene local;
        local.name = node.name;
        local.images = scene.images;
        local.samplers = scene.samplers;
        local.textures = scene.textures;
        local.materials = scene.materials;
        ImportedMesh cookedMesh = scene.meshes.at(*node.mesh);
        Matrix4 publishedTransform = node.worldTransform;
        const RigidScaleDecomposition decomposition = decompose_rigid_scale(node.worldTransform);
        if (decomposition.valid) {
            bake_scale_into_mesh(cookedMesh, decomposition.scale);
            publishedTransform = decomposition.rigid;
        } else if (nodeSettings.generateCollision.value_or(true)) {
            output.diagnostics.push_back({ImportDiagnostic::Severity::Error, "NON_RIGID_NODE_TRANSFORM",
                path + ": node transform contains shear, a projective term, or a singular axis. "
                "Collision-enabled voxel objects require a rotation/translation plus bakeable scale."});
        }
        local.meshes.push_back(std::move(cookedMesh));
        ImportedNode localNode;
        localNode.name = node.name;
        localNode.mesh = 0;
        local.nodes.push_back(localNode);
        local.roots.push_back(0);

        CookedVoxelObject object(voxel.objectId);
        object.name = node.name;
        object.nodePath = path;
        object.sourceNode = nodeIndex;
        object.worldTransform = publishedTransform;
        object.anchored = nodeSettings.anchored.value_or(false);
        object.structural = nodeSettings.structural.value_or(true);
        object.generateCollision = nodeSettings.generateCollision.value_or(true);
        object.asset = voxelize_scene(local, voxel);
        const std::uint32_t objectIndex = static_cast<std::uint32_t>(output.objects.size());
        nodeToObject[nodeIndex] = objectIndex;
        output.objects.push_back(std::move(object));
    }

    for (std::uint32_t objectIndex = 0; objectIndex < output.objects.size(); ++objectIndex) {
        CookedVoxelObject& object = output.objects[objectIndex];
        std::optional<std::uint32_t> parent = scene.nodes.at(object.sourceNode).parent;
        while (parent.has_value()) {
            if (nodeToObject[*parent].has_value()) {
                object.parentObject = nodeToObject[*parent];
                break;
            }
            parent = scene.nodes.at(*parent).parent;
        }
        for (const ImportDiagnostic& diagnostic : object.asset.diagnostics) {
            ImportDiagnostic copy = diagnostic;
            copy.message = object.nodePath + ": " + copy.message;
            output.diagnostics.push_back(std::move(copy));
        }
    }
    bool hasError = output.objects.empty();
    if (output.objects.empty()) {
        output.diagnostics.push_back({ImportDiagnostic::Severity::Error, "NO_COOKED_OBJECTS",
            "No mesh-bearing nodes remained after node import rules were applied."});
    }
    for (const auto& diagnostic : output.diagnostics) hasError = hasError || diagnostic.severity == ImportDiagnostic::Severity::Error;
    output.success = !hasError;
    return output;
}

CookedVoxelScene cook_model_scene(
    const std::filesystem::path& sourcePath,
    const ModelImportOptions& importOptions,
    const SceneCookSettings& settings) {
    ImportedModelResult imported = import_model(sourcePath, importOptions);
    if (!imported.success) {
        CookedVoxelScene failed;
        failed.name = sourcePath.stem().string();
        failed.diagnostics = std::move(imported.diagnostics);
        failed.success = false;
        return failed;
    }
    CookedVoxelScene scene = voxelize_scene_objects(imported.scene, settings);
    scene.diagnostics.insert(scene.diagnostics.begin(), imported.diagnostics.begin(), imported.diagnostics.end());
    for (const auto& diagnostic : scene.diagnostics) {
        if (diagnostic.severity == ImportDiagnostic::Severity::Error) scene.success = false;
    }
    return scene;
}

bool apply_scene_cook_settings_json(
    const std::filesystem::path& path,
    SceneCookSettings& settings,
    std::string* error) {
    try {
        const JsonValue root = JsonParser(read_text_file(path)).parse();
        if (!root.is_object()) throw std::runtime_error("settings root must be a JSON object");
        if (!apply_voxelize_settings_json(path, settings.defaults, error)) return false;
        settings.splitByNode = json_bool(root.find("splitByNode"), settings.splitByNode);
        if (const JsonValue* nodes = root.find("nodes")) {
            if (!nodes->is_object()) throw std::runtime_error("settings.nodes must be an object");
            for (const auto& [name, value] : nodes->object()) {
                NodeCookSettings node;
                apply_node_settings_object(value, node);
                settings.nodeOverrides[name] = node;
            }
        }
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) *error = exception.what();
        return false;
    }
}

[[nodiscard]] std::string sanitize_filename(std::string_view name) {
    std::string output;
    for (const unsigned char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
            output.push_back(static_cast<char>(c));
        } else if (c == ' ' || c == '.' || c == '/') {
            output.push_back('_');
        }
    }
    if (output.empty()) output = "object";
    return output;
}

bool write_dvox_scene_package(
    const std::filesystem::path& manifestPath,
    const CookedVoxelScene& scene,
    std::string* error) {
    try {
        if (!scene.success) throw std::runtime_error("cannot write an unsuccessful cooked scene");
        const std::filesystem::path outputDirectory = manifestPath.parent_path();
        if (!outputDirectory.empty()) std::filesystem::create_directories(outputDirectory);
        std::string packageStem = manifestPath.filename().string();
        constexpr std::string_view sceneSuffix = ".dvoxscene.json";
        if (packageStem.size() >= sceneSuffix.size() &&
            packageStem.compare(packageStem.size() - sceneSuffix.size(), sceneSuffix.size(), sceneSuffix) == 0) {
            packageStem.resize(packageStem.size() - sceneSuffix.size());
        } else {
            packageStem = manifestPath.stem().string();
        }
        if (packageStem.empty()) packageStem = "scene";
        std::vector<std::string> files;
        files.reserve(scene.objects.size());
        for (const CookedVoxelObject& object : scene.objects) {
            std::ostringstream filename;
            filename << packageStem << '_' << sanitize_filename(object.name) << '_'
                     << std::hex << object.asset.object.id() << ".dvox";
            files.push_back(filename.str());
            std::string writeError;
            if (!write_dvox(outputDirectory / files.back(), object.asset, {}, &writeError)) {
                throw std::runtime_error("object DVOX write failed: " + writeError);
            }
        }
        std::ofstream output(manifestPath);
        if (!output) throw std::runtime_error("unable to open DVOX scene manifest");
        output << "{\n  \"format\": \"DVOXSCENE\",\n  \"version\": 1,\n  \"name\": ";
        std::string escaped;
        append_json_escaped(escaped, scene.name);
        output << escaped << ",\n  \"objects\": [\n";
        for (std::size_t i = 0; i < scene.objects.size(); ++i) {
            const CookedVoxelObject& object = scene.objects[i];
            std::string name, path, file;
            append_json_escaped(name, object.name);
            append_json_escaped(path, object.nodePath);
            append_json_escaped(file, files[i]);
            output << "    {\"index\":" << i << ",\"id\":";
            constexpr std::uint64_t maximumExactJsonInteger = 9007199254740991ULL;
            if (object.asset.object.id() <= maximumExactJsonInteger) output << object.asset.object.id();
            else output << '"' << object.asset.object.id() << '"';
            output << ",\"name\":" << name << ",\"nodePath\":" << path << ",\"file\":" << file
                   << ",\"parent\":";
            if (object.parentObject.has_value()) output << *object.parentObject; else output << "null";
            output << ",\"anchored\":" << (object.anchored ? "true" : "false")
                   << ",\"structural\":" << (object.structural ? "true" : "false")
                   << ",\"generateCollision\":" << (object.generateCollision ? "true" : "false")
                   << ",\"worldMatrix\":[";
            for (std::size_t element = 0; element < object.worldTransform.values.size(); ++element) {
                if (element != 0) output << ',';
                output << std::setprecision(9) << object.worldTransform.values[element];
            }
            output << "]}" << (i + 1 == scene.objects.size() ? "\n" : ",\n");
        }
        output << "  ]\n}\n";
        if (!output) throw std::runtime_error("failed while writing DVOX scene manifest");
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) *error = exception.what();
        return false;
    }
}

bool apply_voxelize_settings_json(
    const std::filesystem::path& path,
    VoxelizeSettings& settings,
    std::string* error) {
    try {
        const JsonValue root = JsonParser(read_text_file(path)).parse();
        if (!root.is_object()) throw std::runtime_error("settings root must be a JSON object");
        const JsonValue* values = &root;
        if (const JsonValue* defaults = root.find("defaults")) {
            if (!defaults->is_object()) throw std::runtime_error("settings.defaults must be a JSON object");
            values = defaults;
        }

        const auto find_alias = [&](std::string_view camel, std::string_view snake) -> const JsonValue* {
            if (const JsonValue* value = values->find(camel)) return value;
            return values->find(snake);
        };
        if (const JsonValue* mode = values->find("mode")) {
            const std::string text = json_string(mode);
            if (text == "solid") settings.mode = VoxelizationMode::Solid;
            else if (text == "shell") settings.mode = VoxelizationMode::Shell;
            else if (text == "surface") settings.mode = VoxelizationMode::SurfaceOnly;
            else throw std::runtime_error("settings mode must be solid, shell, or surface");
        }
        settings.voxelSizeMeters = json_float(
            find_alias("voxelSizeMeters", "voxel_size_meters"), settings.voxelSizeMeters);
        settings.shellThicknessVoxels = json_u32(
            find_alias("shellThicknessVoxels", "shell_thickness_voxels"), settings.shellThicknessVoxels);
        settings.supersampleFactor = json_u32(
            find_alias("supersampleFactor", "supersample_factor"), settings.supersampleFactor);
        settings.downsampleCoverageThreshold = json_float(
            find_alias("downsampleCoverageThreshold", "downsample_coverage_threshold"),
            settings.downsampleCoverageThreshold);
        const std::uint32_t interior = json_u32(
            find_alias("interiorMaterial", "interior_material"), settings.interiorMaterial);
        if (interior > std::numeric_limits<MaterialId>::max()) {
            throw std::runtime_error("interiorMaterial exceeds MaterialId range");
        }
        settings.interiorMaterial = static_cast<MaterialId>(interior);
        settings.objectId = json_u64(find_alias("objectId", "object_id"), settings.objectId);
        settings.maximumWorkingVoxels = json_u64(
            find_alias("maximumWorkingVoxels", "maximum_working_voxels"), settings.maximumWorkingVoxels);
        settings.preserveThinSurface = json_bool(
            find_alias("preserveThinSurface", "preserve_thin_surface"), settings.preserveThinSurface);
        settings.maximumPaletteMaterials = json_u32(
            find_alias("maximumPaletteMaterials", "maximum_palette_materials"), settings.maximumPaletteMaterials);

        if (!(settings.voxelSizeMeters > 0.0F) || !std::isfinite(settings.voxelSizeMeters)) {
            throw std::runtime_error("voxelSizeMeters must be finite and positive");
        }
        if (settings.supersampleFactor != 1 && settings.supersampleFactor != 2 &&
            settings.supersampleFactor != 4) {
            throw std::runtime_error("supersampleFactor must be 1, 2, or 4");
        }
        if (!(settings.downsampleCoverageThreshold > 0.0F &&
              settings.downsampleCoverageThreshold <= 1.0F)) {
            throw std::runtime_error("downsampleCoverageThreshold must be in (0, 1]");
        }
        if (settings.maximumWorkingVoxels == 0) {
            throw std::runtime_error("maximumWorkingVoxels must be positive");
        }
        if (settings.maximumPaletteMaterials < 2 || settings.maximumPaletteMaterials > 256) {
            throw std::runtime_error("maximumPaletteMaterials must be in [2, 256]");
        }
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) *error = exception.what();
        return false;
    }
}

bool write_import_report_html(
    const std::filesystem::path& path,
    std::string_view sourceName,
    const CookedVoxelAsset& asset,
    std::string* error) {
    try {
        std::ofstream output(path);
        if (!output) throw std::runtime_error("unable to open report output");
        output << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
                  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                  "<title>DVE Import Report</title><style>"
                  "body{font:15px/1.5 system-ui;background:#0b1020;color:#edf3ff;margin:0;padding:32px}"
                  "main{max-width:1050px;margin:auto}.card{background:#141e34;border:1px solid #30415f;border-radius:12px;padding:18px;margin:14px 0}"
                  "table{border-collapse:collapse;width:100%}th,td{border:1px solid #30415f;padding:8px;text-align:left}"
                  ".Error{color:#fb7185}.Warning{color:#fbbf24}.Info{color:#86efac}code{color:#8bd5ff}</style></head><body><main>";
        output << "<h1>DVE model import report</h1><p><code>" << html_escape(sourceName) << "</code></p>";
        output << "<div class=\"card\"><h2>Cooked asset</h2><table>";
        output << "<tr><th>Voxel size</th><td>" << asset.voxelSizeMeters << " m</td></tr>";
        output << "<tr><th>Source vertices</th><td>" << asset.stats.sourceVertices << "</td></tr>";
        output << "<tr><th>Source triangles</th><td>" << asset.stats.sourceTriangles << "</td></tr>";
        output << "<tr><th>Boundary edges</th><td>" << asset.stats.boundaryEdges << "</td></tr>";
        output << "<tr><th>Non-manifold edges</th><td>" << asset.stats.nonManifoldEdges << "</td></tr>";
        output << "<tr><th>Mesh islands</th><td>" << asset.stats.meshIslands << "</td></tr>";
        output << "<tr><th>Decoded images</th><td>" << asset.stats.decodedImages << "</td></tr>";
        output << "<tr><th>Decoded image bytes</th><td>" << asset.stats.decodedImageBytes << "</td></tr>";
        output << "<tr><th>Texture samples</th><td>" << asset.stats.textureSamples << "</td></tr>";
        output << "<tr><th>Alpha-rejected samples</th><td>" << asset.stats.alphaRejectedSamples << "</td></tr>";
        output << "<tr><th>Unique material samples</th><td>" << asset.stats.uniqueMaterialSamples << "</td></tr>";
        output << "<tr><th>Palette materials</th><td>" << asset.stats.paletteMaterials << "</td></tr>";
        output << "<tr><th>Output voxels</th><td>" << asset.stats.outputVoxels << "</td></tr>";
        output << "<tr><th>Output bricks</th><td>" << asset.stats.outputBricks << "</td></tr>";
        output << "<tr><th>State hash</th><td><code>" << asset.object.state_hash() << "</code></td></tr>";
        output << "</table></div><div class=\"card\"><h2>Diagnostics</h2>";
        if (asset.diagnostics.empty()) output << "<p class=\"Info\">No diagnostics.</p>";
        for (const ImportDiagnostic& diagnostic : asset.diagnostics) {
            output << "<p class=\"" << to_string(diagnostic.severity) << "\"><strong>"
                   << html_escape(diagnostic.code) << ":</strong> " << html_escape(diagnostic.message) << "</p>";
        }
        output << "</div></main></body></html>";
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) *error = exception.what();
        return false;
    }
}

const char* to_string(VoxelizationMode mode) noexcept {
    switch (mode) {
    case VoxelizationMode::Solid: return "solid";
    case VoxelizationMode::Shell: return "shell";
    case VoxelizationMode::SurfaceOnly: return "surface";
    }
    return "unknown";
}

const char* to_string(ImportDiagnostic::Severity severity) noexcept {
    switch (severity) {
    case ImportDiagnostic::Severity::Info: return "Info";
    case ImportDiagnostic::Severity::Warning: return "Warning";
    case ImportDiagnostic::Severity::Error: return "Error";
    }
    return "Unknown";
}

} // namespace dve
