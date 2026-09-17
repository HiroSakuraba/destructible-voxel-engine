#include "dve/runtime_scene.hpp"
#include "dve/master_material.hpp"

#include "dve/job_system.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <future>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace dve {

struct RuntimeSceneParserAccess {
    [[nodiscard]] static RuntimeScene parse_manifest(
        const std::filesystem::path& manifestPath,
        const RuntimeSceneLoadOptions& options);
};

namespace {

using namespace std::string_view_literals;

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr double kMaximumExactJsonInteger = 9007199254740991.0; // 2^53 - 1

struct SceneException final : std::runtime_error {
    RuntimeSceneError error;

    SceneException(RuntimeSceneErrorCode code, std::string message,
                   std::filesystem::path path = {},
                   std::optional<std::uint64_t> objectId = std::nullopt)
        : std::runtime_error(message), error{code, std::move(message), std::move(path), objectId} {}
};

[[noreturn]] void fail(RuntimeSceneErrorCode code, std::string message,
                       const std::filesystem::path& path = {},
                       std::optional<std::uint64_t> objectId = std::nullopt) {
    throw SceneException(code, std::move(message), path, objectId);
}

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

    [[nodiscard]] JsonValue parse() {
        skip_whitespace();
        JsonValue value = parse_value();
        skip_whitespace();
        if (position_ != text_.size()) parse_fail("trailing characters after JSON value");
        return value;
    }

private:
    std::string_view text_;
    std::size_t position_{};

    [[noreturn]] void parse_fail(std::string_view message) const {
        fail(RuntimeSceneErrorCode::Json,
             "JSON parse error at byte " + std::to_string(position_) + ": " + std::string(message));
    }

    void skip_whitespace() {
        while (position_ < text_.size()) {
            const char c = text_[position_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++position_;
        }
    }

    [[nodiscard]] char consume() {
        if (position_ >= text_.size()) parse_fail("unexpected end of input");
        return text_[position_++];
    }

    [[nodiscard]] bool consume_if(char expected) {
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect_literal(std::string_view literal) {
        if (text_.substr(position_, literal.size()) != literal) parse_fail("invalid literal");
        position_ += literal.size();
    }

    [[nodiscard]] JsonValue parse_value() {
        skip_whitespace();
        if (position_ >= text_.size()) parse_fail("expected value");
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

    [[nodiscard]] std::uint32_t parse_hex4() {
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = consume();
            value <<= 4U;
            if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
            else parse_fail("invalid unicode escape");
        }
        return value;
    }

    [[nodiscard]] std::string parse_string() {
        if (consume() != '"') parse_fail("expected string");
        std::string output;
        while (position_ < text_.size()) {
            const char c = consume();
            if (c == '"') return output;
            if (static_cast<unsigned char>(c) < 0x20U) parse_fail("control character in string");
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
                    if (consume() != '\\' || consume() != 'u') parse_fail("expected low surrogate");
                    const std::uint32_t low = parse_hex4();
                    if (low < 0xDC00U || low > 0xDFFFU) parse_fail("invalid low surrogate");
                    codepoint = 0x10000U + ((codepoint - 0xD800U) << 10U) + (low - 0xDC00U);
                } else if (codepoint >= 0xDC00U && codepoint <= 0xDFFFU) {
                    parse_fail("unexpected low surrogate");
                }
                append_utf8(output, codepoint);
                break;
            }
            default: parse_fail("invalid string escape");
            }
        }
        parse_fail("unterminated string");
    }

    [[nodiscard]] double parse_number() {
        const std::size_t start = position_;
        (void)consume_if('-');
        if (position_ >= text_.size()) parse_fail("invalid number");
        if (consume_if('0')) {
        } else {
            if (text_[position_] < '1' || text_[position_] > '9') parse_fail("invalid number");
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
        }
        if (consume_if('.')) {
            const std::size_t fractionStart = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
            if (fractionStart == position_) parse_fail("invalid fraction");
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
            const std::size_t exponentStart = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
            if (exponentStart == position_) parse_fail("invalid exponent");
        }
        const std::string token(text_.substr(start, position_ - start));
        char* end = nullptr;
        const double value = std::strtod(token.c_str(), &end);
        if (end == nullptr || static_cast<std::size_t>(end - token.c_str()) != token.size() || !std::isfinite(value)) {
            parse_fail("number is not finite");
        }
        return value;
    }

    [[nodiscard]] JsonValue::Array parse_array() {
        if (consume() != '[') parse_fail("expected array");
        JsonValue::Array output;
        skip_whitespace();
        if (consume_if(']')) return output;
        for (;;) {
            output.push_back(parse_value());
            skip_whitespace();
            if (consume_if(']')) return output;
            if (!consume_if(',')) parse_fail("expected comma in array");
            skip_whitespace();
        }
    }

    [[nodiscard]] JsonValue::Object parse_object() {
        if (consume() != '{') parse_fail("expected object");
        JsonValue::Object output;
        skip_whitespace();
        if (consume_if('}')) return output;
        for (;;) {
            if (position_ >= text_.size() || text_[position_] != '"') parse_fail("expected object key");
            std::string key = parse_string();
            skip_whitespace();
            if (!consume_if(':')) parse_fail("expected colon after object key");
            skip_whitespace();
            const auto [it, inserted] = output.emplace(std::move(key), parse_value());
            (void)it;
            if (!inserted) parse_fail("duplicate object key");
            skip_whitespace();
            if (consume_if('}')) return output;
            if (!consume_if(',')) parse_fail("expected comma in object");
            skip_whitespace();
        }
    }
};

[[nodiscard]] std::string read_text_file_limited(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) fail(RuntimeSceneErrorCode::Io, "unable to determine manifest size", path);
    if (size > maximumBytes) fail(RuntimeSceneErrorCode::LimitExceeded, "manifest exceeds configured size limit", path);
    std::ifstream input(path, std::ios::binary);
    if (!input) fail(RuntimeSceneErrorCode::Io, "unable to open scene manifest", path);
    std::string text(static_cast<std::size_t>(size), '\0');
    if (!text.empty() && !input.read(text.data(), static_cast<std::streamsize>(text.size()))) {
        fail(RuntimeSceneErrorCode::Io, "unable to read scene manifest", path);
    }
    return text;
}

[[nodiscard]] const JsonValue& require_field(
    const JsonValue& object,
    std::string_view key,
    std::string_view context) {
    if (!object.is_object()) fail(RuntimeSceneErrorCode::InvalidManifest, std::string(context) + " must be an object");
    const JsonValue* value = object.find(key);
    if (value == nullptr) fail(RuntimeSceneErrorCode::InvalidManifest,
                               std::string(context) + " is missing required field '" + std::string(key) + "'");
    return *value;
}

void reject_unknown_fields(
    const JsonValue& object,
    std::span<const std::string_view> allowed,
    std::string_view context) {
    if (!object.is_object()) fail(RuntimeSceneErrorCode::InvalidManifest, std::string(context) + " must be an object");
    for (const auto& [key, value] : object.object()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            fail(RuntimeSceneErrorCode::InvalidManifest,
                 std::string(context) + " contains unknown field '" + key + "'");
        }
    }
}

[[nodiscard]] std::string require_string(
    const JsonValue& value,
    std::string_view context,
    std::size_t maximumLength = 4096U) {
    if (!value.is_string()) fail(RuntimeSceneErrorCode::InvalidManifest, std::string(context) + " must be a string");
    if (value.string().size() > maximumLength) {
        fail(RuntimeSceneErrorCode::LimitExceeded, std::string(context) + " exceeds its length limit");
    }
    return value.string();
}

[[nodiscard]] bool require_bool(const JsonValue& value, std::string_view context) {
    if (!value.is_bool()) fail(RuntimeSceneErrorCode::InvalidManifest, std::string(context) + " must be a boolean");
    return value.boolean();
}

[[nodiscard]] std::uint64_t parse_u64(const JsonValue& value, std::string_view context) {
    if (value.is_number()) {
        const double number = value.number();
        if (number < 0.0 || number > kMaximumExactJsonInteger || std::floor(number) != number) {
            fail(RuntimeSceneErrorCode::InvalidManifest,
                 std::string(context) + " must be an exact non-negative integer; use a decimal string above 2^53-1");
        }
        return static_cast<std::uint64_t>(number);
    }
    if (value.is_string()) {
        if (value.string().empty()) fail(RuntimeSceneErrorCode::InvalidManifest, std::string(context) + " is empty");
        std::uint64_t result = 0;
        const char* begin = value.string().data();
        const char* end = begin + value.string().size();
        const auto parsed = std::from_chars(begin, end, result, 10);
        if (parsed.ec != std::errc{} || parsed.ptr != end) {
            fail(RuntimeSceneErrorCode::InvalidManifest, std::string(context) + " is not a valid uint64 decimal string");
        }
        return result;
    }
    fail(RuntimeSceneErrorCode::InvalidManifest, std::string(context) + " must be an integer or decimal string");
}

[[nodiscard]] std::size_t parse_index(const JsonValue& value, std::string_view context) {
    const std::uint64_t integer = parse_u64(value, context);
    if (integer > std::numeric_limits<std::size_t>::max()) {
        fail(RuntimeSceneErrorCode::LimitExceeded, std::string(context) + " exceeds size_t");
    }
    return static_cast<std::size_t>(integer);
}

[[nodiscard]] float require_float(const JsonValue& value, std::string_view context) {
    if (!value.is_number() || !std::isfinite(value.number()) ||
        value.number() < -static_cast<double>(std::numeric_limits<float>::max()) ||
        value.number() > static_cast<double>(std::numeric_limits<float>::max())) {
        fail(RuntimeSceneErrorCode::InvalidManifest, std::string(context) + " must be a finite float");
    }
    return static_cast<float>(value.number());
}

[[nodiscard]] float vector_length(Float3 value) noexcept {
    return std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
}

[[nodiscard]] Float3 cross_product(Float3 a, Float3 b) noexcept {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

[[nodiscard]] Quaternion quaternion_from_rotation_columns(Float3 c0, Float3 c1, Float3 c2) noexcept {
    // Convert the column-major rotation basis to the conventional row-indexed formula.
    const float m00 = c0.x, m01 = c1.x, m02 = c2.x;
    const float m10 = c0.y, m11 = c1.y, m12 = c2.y;
    const float m20 = c0.z, m21 = c1.z, m22 = c2.z;
    const float trace = m00 + m11 + m22;
    Quaternion q;
    if (trace > 0.0F) {
        const float s = std::sqrt(trace + 1.0F) * 2.0F;
        q.w = 0.25F * s;
        q.x = (m21 - m12) / s;
        q.y = (m02 - m20) / s;
        q.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0F + m00 - m11 - m22) * 2.0F;
        q.w = (m21 - m12) / s;
        q.x = 0.25F * s;
        q.y = (m01 + m10) / s;
        q.z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0F + m11 - m00 - m22) * 2.0F;
        q.w = (m02 - m20) / s;
        q.x = (m01 + m10) / s;
        q.y = 0.25F * s;
        q.z = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0F + m22 - m00 - m11) * 2.0F;
        q.w = (m10 - m01) / s;
        q.x = (m02 + m20) / s;
        q.y = (m12 + m21) / s;
        q.z = 0.25F * s;
    }
    return normalize(q);
}

[[nodiscard]] RigidTransform parse_rigid_matrix(const JsonValue& value, std::uint64_t objectId) {
    if (!value.is_array() || value.array().size() != 16U) {
        fail(RuntimeSceneErrorCode::InvalidTransform, "worldMatrix must contain exactly 16 numbers", {}, objectId);
    }
    std::array<float, 16> m{};
    for (std::size_t i = 0; i < m.size(); ++i) {
        m[i] = require_float(value.array()[i], "worldMatrix element");
    }
    constexpr float affineTolerance = 1.0e-5F;
    if (std::abs(m[3]) > affineTolerance || std::abs(m[7]) > affineTolerance ||
        std::abs(m[11]) > affineTolerance || std::abs(m[15] - 1.0F) > affineTolerance) {
        fail(RuntimeSceneErrorCode::InvalidTransform, "worldMatrix is not affine", {}, objectId);
    }
    const Float3 c0{m[0], m[1], m[2]};
    const Float3 c1{m[4], m[5], m[6]};
    const Float3 c2{m[8], m[9], m[10]};
    constexpr float orthonormalTolerance = 2.0e-4F;
    if (std::abs(vector_length(c0) - 1.0F) > orthonormalTolerance ||
        std::abs(vector_length(c1) - 1.0F) > orthonormalTolerance ||
        std::abs(vector_length(c2) - 1.0F) > orthonormalTolerance ||
        std::abs(dot(c0, c1)) > orthonormalTolerance ||
        std::abs(dot(c0, c2)) > orthonormalTolerance ||
        std::abs(dot(c1, c2)) > orthonormalTolerance) {
        fail(RuntimeSceneErrorCode::InvalidTransform,
             "worldMatrix must contain a rigid orthonormal basis; scale and shear must be baked by the cooker", {}, objectId);
    }
    const float determinant = dot(c0, cross_product(c1, c2));
    if (std::abs(determinant - 1.0F) > 5.0e-4F) {
        fail(RuntimeSceneErrorCode::InvalidTransform, "worldMatrix must be right-handed with determinant +1", {}, objectId);
    }
    return make_rigid_transform({m[12], m[13], m[14]}, quaternion_from_rotation_columns(c0, c1, c2));
}

[[nodiscard]] bool path_has_parent_reference(const std::filesystem::path& path) {
    for (const auto& component : path) {
        if (component == "..") return true;
    }
    return false;
}

[[nodiscard]] bool path_is_within(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate) {
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end() || *rootIt != *candidateIt) return false;
    }
    return true;
}

[[nodiscard]] std::filesystem::path resolve_contained_asset(
    const std::filesystem::path& packageRoot,
    const std::string& file,
    std::uint64_t objectId,
    std::uint64_t maximumBytes) {
    const std::filesystem::path relative = std::filesystem::path(file).lexically_normal();
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() ||
        path_has_parent_reference(relative)) {
        fail(RuntimeSceneErrorCode::PathEscape, "object asset path is not a contained relative path", relative, objectId);
    }
    if (relative.extension() != ".dvox") {
        fail(RuntimeSceneErrorCode::InvalidManifest, "object asset file must use the .dvox extension", relative, objectId);
    }
    std::error_code ec;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(packageRoot / relative, ec);
    if (ec || !path_is_within(packageRoot, resolved)) {
        fail(RuntimeSceneErrorCode::PathEscape, "object asset resolves outside the scene package root", relative, objectId);
    }
    if (!std::filesystem::is_regular_file(resolved, ec) || ec) {
        fail(RuntimeSceneErrorCode::MissingAsset, "referenced DVOX asset is missing or not a regular file", resolved, objectId);
    }
    const std::uintmax_t size = std::filesystem::file_size(resolved, ec);
    if (ec) fail(RuntimeSceneErrorCode::Io, "unable to determine DVOX asset size", resolved, objectId);
    if (size > maximumBytes) {
        fail(RuntimeSceneErrorCode::LimitExceeded, "DVOX asset exceeds configured per-object size limit", resolved, objectId);
    }
    return resolved;
}

void validate_hierarchy(std::span<const RuntimeSceneObject> objects) {
    std::vector<std::uint8_t> color(objects.size(), 0U);
    const auto visit = [&](auto&& self, std::size_t index) -> void {
        if (color[index] == 1U) {
            fail(RuntimeSceneErrorCode::CyclicHierarchy, "scene object hierarchy contains a cycle", {}, objects[index].metadata().id);
        }
        if (color[index] == 2U) return;
        color[index] = 1U;
        if (objects[index].metadata().parentIndex) self(self, *objects[index].metadata().parentIndex);
        color[index] = 2U;
    };
    for (std::size_t i = 0; i < objects.size(); ++i) visit(visit, i);
}

} // namespace

RuntimeScene RuntimeSceneParserAccess::parse_manifest(
    const std::filesystem::path& manifestPath,
    const RuntimeSceneLoadOptions& options) {
    std::error_code ec;
    const std::filesystem::path canonicalManifest = std::filesystem::weakly_canonical(manifestPath, ec);
    if (ec || !std::filesystem::is_regular_file(canonicalManifest, ec) || ec) {
        fail(RuntimeSceneErrorCode::Io, "scene manifest is missing or not a regular file", manifestPath);
    }
    const std::filesystem::path packageRoot = std::filesystem::weakly_canonical(canonicalManifest.parent_path(), ec);
    if (ec) fail(RuntimeSceneErrorCode::Io, "unable to canonicalize scene package root", canonicalManifest);

    const JsonValue root = JsonParser(read_text_file_limited(canonicalManifest, options.maximumManifestBytes)).parse();
    static constexpr std::array rootFields{"format"sv, "version"sv, "name"sv, "objects"sv};
    reject_unknown_fields(root, rootFields, "manifest root");
    const std::string format = require_string(require_field(root, "format", "manifest root"), "manifest.format", 32U);
    if (format != "DVOXSCENE") fail(RuntimeSceneErrorCode::InvalidManifest, "manifest.format must be DVOXSCENE", canonicalManifest);
    const std::uint64_t version = parse_u64(require_field(root, "version", "manifest root"), "manifest.version");
    if (version != 1U) fail(RuntimeSceneErrorCode::UnsupportedVersion, "only DVOXSCENE version 1 is supported", canonicalManifest);
    const std::string name = require_string(require_field(root, "name", "manifest root"), "manifest.name", 1024U);
    const JsonValue& objectArray = require_field(root, "objects", "manifest root");
    if (!objectArray.is_array()) fail(RuntimeSceneErrorCode::InvalidManifest, "manifest.objects must be an array", canonicalManifest);
    if (objectArray.array().size() > options.maximumObjects) {
        fail(RuntimeSceneErrorCode::LimitExceeded, "manifest object count exceeds configured limit", canonicalManifest);
    }

    RuntimeScene scene;
    scene.name_ = name;
    scene.manifestPath_ = canonicalManifest;
    scene.packageRoot_ = packageRoot;
    scene.options_ = options;
    scene.objects_.reserve(objectArray.array().size());
    std::set<std::uint64_t> ids;
    std::set<std::size_t> indices;
    std::set<std::filesystem::path> assetPaths;
    static constexpr std::array objectFields{
        "index"sv, "id"sv, "name"sv, "nodePath"sv, "file"sv, "parent"sv,
        "anchored"sv, "structural"sv, "generateCollision"sv, "worldMatrix"sv};

    for (std::size_t arrayIndex = 0; arrayIndex < objectArray.array().size(); ++arrayIndex) {
        const JsonValue& value = objectArray.array()[arrayIndex];
        reject_unknown_fields(value, objectFields, "manifest object");
        RuntimeSceneObject object;
        object.metadata_.index = parse_index(require_field(value, "index", "manifest object"), "object.index");
        object.metadata_.id = parse_u64(require_field(value, "id", "manifest object"), "object.id");
        if (object.metadata_.id == 0U) fail(RuntimeSceneErrorCode::InvalidManifest, "object.id must be nonzero", {}, 0U);
        object.metadata_.name = require_string(require_field(value, "name", "manifest object"), "object.name", 1024U);
        object.metadata_.nodePath = require_string(require_field(value, "nodePath", "manifest object"), "object.nodePath", 4096U);
        const std::string file = require_string(require_field(value, "file", "manifest object"), "object.file", 4096U);
        object.metadata_.relativeFile = std::filesystem::path(file).lexically_normal();
        const JsonValue& parent = require_field(value, "parent", "manifest object");
        if (!parent.is_null()) object.metadata_.parentIndex = parse_index(parent, "object.parent");
        object.metadata_.anchored = require_bool(require_field(value, "anchored", "manifest object"), "object.anchored");
        object.metadata_.structural = require_bool(require_field(value, "structural", "manifest object"), "object.structural");
        object.metadata_.generateCollision = require_bool(require_field(value, "generateCollision", "manifest object"), "object.generateCollision");
        object.metadata_.worldTransform = parse_rigid_matrix(require_field(value, "worldMatrix", "manifest object"), object.metadata_.id);
        if (!ids.insert(object.metadata_.id).second) {
            fail(RuntimeSceneErrorCode::DuplicateObjectId, "manifest contains duplicate object ID", {}, object.metadata_.id);
        }
        if (!indices.insert(object.metadata_.index).second) {
            fail(RuntimeSceneErrorCode::DuplicateObjectIndex, "manifest contains duplicate object index", {}, object.metadata_.id);
        }
        object.resolvedAssetPath_ = resolve_contained_asset(packageRoot, file, object.metadata_.id,
                                                            options.maximumDvoxBytesPerObject);
        if (!assetPaths.insert(object.resolvedAssetPath_).second) {
            fail(RuntimeSceneErrorCode::DuplicateAssetPath,
                 "multiple manifest objects reference the same canonical DVOX file", object.resolvedAssetPath_, object.metadata_.id);
        }
        scene.objects_.push_back(std::move(object));
    }

    std::sort(scene.objects_.begin(), scene.objects_.end(), [](const RuntimeSceneObject& a, const RuntimeSceneObject& b) {
        return a.metadata().index < b.metadata().index;
    });
    for (std::size_t i = 0; i < scene.objects_.size(); ++i) {
        if (scene.objects_[i].metadata_.index != i) {
            fail(RuntimeSceneErrorCode::DuplicateObjectIndex,
                 "manifest object indices must form the contiguous range [0, objectCount)", {}, scene.objects_[i].metadata_.id);
        }
    }
    for (RuntimeSceneObject& object : scene.objects_) {
        if (!object.metadata_.parentIndex) continue;
        const std::size_t parent = *object.metadata_.parentIndex;
        if (parent >= scene.objects_.size() || parent == object.metadata_.index) {
            fail(RuntimeSceneErrorCode::InvalidParent, "object parent index is invalid", {}, object.metadata_.id);
        }
        object.metadata_.parentId = scene.objects_[parent].metadata_.id;
    }
    validate_hierarchy(scene.objects_);
    return scene;
}

namespace {

[[nodiscard]] bool finite(float value) noexcept { return std::isfinite(value); }

[[nodiscard]] bool validate_material(const VoxelMaterialDefinition& material) noexcept {
    if (!(finite(material.baseColor.x) && finite(material.baseColor.y) && finite(material.baseColor.z) &&
          finite(material.baseColor.w) && finite(material.emissive.x) && finite(material.emissive.y) &&
          finite(material.emissive.z) && finite(material.metallic) && finite(material.roughness) &&
          finite(material.specular) && finite(material.subsurfaceScatterDistanceMeters) &&
          finite(material.subsurfaceColor.x) && finite(material.subsurfaceColor.y) && finite(material.subsurfaceColor.z) &&
          finite(material.clearCoat) && finite(material.clearCoatRoughness) && finite(material.foliageColor.x) &&
          finite(material.foliageColor.y) && finite(material.foliageColor.z) && finite(material.foliageTransmittance) &&
          finite(material.foliageWrap) && finite(material.densityKilogramsPerCubicMeter) &&
          material.densityKilogramsPerCubicMeter >= 0.0F && finite(material.structuralStrength) &&
          finite(material.fractureResistance) && finite(material.flammability) && finite(material.thermalConductivity)))
        return false;
    if (material.metallic < 0.0F || material.metallic > 1.0F || material.roughness < 0.0F || material.roughness > 1.0F ||
        material.specular < 0.0F || material.specular > 1.0F || material.clearCoat < 0.0F || material.clearCoat > 1.0F ||
        material.clearCoatRoughness < 0.0F || material.clearCoatRoughness > 1.0F ||
        material.foliageTransmittance < 0.0F || material.foliageTransmittance > 1.0F ||
        material.foliageWrap < 0.0F || material.foliageWrap > 1.0F ||
        material.layers.size() > kMaximumVoxelMaterialLayers ||
        static_cast<unsigned>(material.shadingModel) > static_cast<unsigned>(MaterialShadingModel::ClearCoat) ||
        static_cast<unsigned>(material.blendMode) > static_cast<unsigned>(MaterialBlendMode::Translucent)) return false;
    for (const VoxelMaterialLayer& layer : material.layers) {
        if (!finite(layer.weight) || layer.weight < 0.0F || layer.weight > 1.0F ||
            static_cast<unsigned>(layer.blendMode) > static_cast<unsigned>(MaterialLayerBlendMode::Additive)) return false;
    }
    return true;
}

[[nodiscard]] bool validate_material_references(const CookedVoxelAsset& asset) {
    if (asset.materials.empty() || asset.materials.size() > 256U) return false;
    for (const VoxelMaterialDefinition& material : asset.materials) {
        for (const VoxelMaterialLayer& layer : material.layers) {
            if (layer.sourceMaterial >= asset.materials.size()) return false;
        }
    }
    std::string layerError;
    if (!resolve_voxel_material_layers(asset.materials, &layerError)) return false;
    for (const auto& [key, brick] : asset.object.bricks()) {
        (void)key;
        const Bitset512 occupancy = brick.occupancy();
        bool valid = true;
        occupancy.for_each_set([&](std::uint16_t index) {
            if (brick.material(index) >= asset.materials.size()) valid = false;
        });
        if (!valid) return false;
    }
    return true;
}

[[nodiscard]] MaterialMassTable make_material_mass_table(
    const CookedVoxelAsset& asset,
    double& densityQuantumKilogramsPerCubicMeter) {
    double maximumDensity = 0.0;
    for (const VoxelMaterialDefinition& material : asset.materials) {
        maximumDensity = std::max(maximumDensity, static_cast<double>(material.densityKilogramsPerCubicMeter));
    }
    if (!(maximumDensity > 0.0) || !std::isfinite(maximumDensity)) {
        fail(RuntimeSceneErrorCode::DerivedDataFailure,
             "collision-enabled dynamic object has no positive finite material density", {}, asset.object.id());
    }
    densityQuantumKilogramsPerCubicMeter = maximumDensity / static_cast<double>(kMaximumMaterialDensityUnits);
    MaterialMassTable table;
    const std::size_t count = std::min<std::size_t>(asset.materials.size(), 256U);
    for (std::size_t i = 1; i < count; ++i) {
        const double density = static_cast<double>(asset.materials[i].densityKilogramsPerCubicMeter);
        std::uint16_t units = 0;
        if (density > 0.0) {
            const long rounded = std::lround(density / densityQuantumKilogramsPerCubicMeter);
            units = static_cast<std::uint16_t>(std::clamp<long>(rounded, 1L, kMaximumMaterialDensityUnits));
        }
        table.set_density_units(static_cast<MaterialId>(i), units);
    }
    return table;
}

void hash_bytes(std::uint64_t& hash, const void* data, std::size_t bytes) noexcept {
    const auto* input = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
        hash ^= input[i];
        hash *= kFnvPrime;
    }
}

template <typename T>
void hash_value(std::uint64_t& hash, const T& value) noexcept {
    hash_bytes(hash, &value, sizeof(value));
}

void hash_string(std::uint64_t& hash, std::string_view value) noexcept {
    const std::uint64_t size = value.size();
    hash_value(hash, size);
    hash_bytes(hash, value.data(), value.size());
}

void hash_transform(std::uint64_t& hash, const RigidTransform& transform) noexcept {
    hash_value(hash, std::bit_cast<std::uint32_t>(transform.position.x));
    hash_value(hash, std::bit_cast<std::uint32_t>(transform.position.y));
    hash_value(hash, std::bit_cast<std::uint32_t>(transform.position.z));
    hash_value(hash, std::bit_cast<std::uint32_t>(transform.rotation.x));
    hash_value(hash, std::bit_cast<std::uint32_t>(transform.rotation.y));
    hash_value(hash, std::bit_cast<std::uint32_t>(transform.rotation.z));
    hash_value(hash, std::bit_cast<std::uint32_t>(transform.rotation.w));
}

[[nodiscard]] std::uint64_t hash_cooked_asset(const CookedVoxelAsset& asset) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_value(hash, std::bit_cast<std::uint32_t>(asset.voxelSizeMeters));
    const std::uint64_t authorityHash = asset.object.state_hash();
    hash_value(hash, authorityHash);
    const std::uint64_t materialCount = asset.materials.size();
    hash_value(hash, materialCount);
    for (const VoxelMaterialDefinition& material : asset.materials) {
        hash_string(hash, material.name);
        hash_value(hash, std::bit_cast<std::uint32_t>(material.baseColor.x));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.baseColor.y));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.baseColor.z));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.baseColor.w));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.emissive.x));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.emissive.y));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.emissive.z));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.metallic));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.roughness));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.specular));
        hash_value(hash, material.shadingModel);
        hash_value(hash, material.blendMode);
        hash_value(hash, std::bit_cast<std::uint32_t>(material.subsurfaceScatterDistanceMeters));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.subsurfaceColor.x));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.subsurfaceColor.y));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.subsurfaceColor.z));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.clearCoat));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.clearCoatRoughness));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.foliageColor.x));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.foliageColor.y));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.foliageColor.z));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.foliageTransmittance));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.foliageWrap));
        const std::uint64_t layerCount = material.layers.size();
        hash_value(hash, layerCount);
        for (const VoxelMaterialLayer& layer : material.layers) {
            hash_value(hash, layer.sourceMaterial);
            hash_value(hash, std::bit_cast<std::uint32_t>(layer.weight));
            hash_value(hash, layer.blendMode);
            hash_value(hash, layer.enabled);
        }
        hash_value(hash, std::bit_cast<std::uint32_t>(material.densityKilogramsPerCubicMeter));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.structuralStrength));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.fractureResistance));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.flammability));
        hash_value(hash, std::bit_cast<std::uint32_t>(material.thermalConductivity));
        hash_value(hash, material.transparent);
        hash_value(hash, material.structural);
    }
    return hash;
}

} // namespace

RuntimeSceneObjectStats RuntimeSceneObject::stats() const noexcept {
    RuntimeSceneObjectStats result;
    if (!asset_) return result;
    result.voxels = asset_->object.occupied_voxel_count();
    result.bricks = asset_->object.brick_count();
    result.materialCount = asset_->materials.size();
    if (connectivity_) result.connectivityComponents = connectivity_->object_component_count();
    result.collisionBoxes = collisionBoxes_.size();
    result.collisionProxyOverBudget = collisionProxyOverBudget_;
    result.authorityHash = asset_->object.state_hash();
    result.assetContentHash = hash_cooked_asset(*asset_);
    if (packedBrickmap_) result.packedBrickmapHash = packedBrickmap_->readback_hash();
    result.rendererReadbackHash = rendererReadbackHash_;
    return result;
}

std::size_t RuntimeScene::loaded_object_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(objects_.begin(), objects_.end(),
                                                  [](const RuntimeSceneObject& object) { return object.loaded(); }));
}

const RuntimeSceneObject* RuntimeScene::find_object(std::uint64_t objectId) const noexcept {
    for (const RuntimeSceneObject& object : objects_) {
        if (object.metadata().id == objectId) return &object;
    }
    return nullptr;
}

std::uint64_t RuntimeScene::state_hash() const noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_string(hash, name_);
    hash_value(hash, std::bit_cast<std::uint32_t>(uniformVoxelSizeMeters_));
    hash_value(hash, mixedVoxelSizes_);
    const std::uint64_t count = objects_.size();
    hash_value(hash, count);
    for (const RuntimeSceneObject& object : objects_) {
        const RuntimeSceneObjectMetadata& metadata = object.metadata();
        hash_value(hash, metadata.index);
        hash_value(hash, metadata.id);
        hash_string(hash, metadata.name);
        hash_string(hash, metadata.nodePath);
        hash_string(hash, metadata.relativeFile.generic_string());
        const std::uint64_t parent = metadata.parentId.value_or(0U);
        hash_value(hash, parent);
        hash_value(hash, metadata.anchored);
        hash_value(hash, metadata.structural);
        hash_value(hash, metadata.generateCollision);
        hash_transform(hash, metadata.worldTransform);
        const bool loaded = object.loaded();
        hash_value(hash, loaded);
        if (loaded) {
            const RuntimeSceneObjectStats objectStats = object.stats();
            hash_value(hash, objectStats.authorityHash);
            hash_value(hash, objectStats.assetContentHash);
            hash_value(hash, objectStats.packedBrickmapHash);
            hash_value(hash, objectStats.connectivityComponents);
            hash_value(hash, objectStats.collisionBoxes);
        }
    }
    return hash;
}


RuntimeSceneWorld::~RuntimeSceneWorld() {
    for (SceneSlot& slot : scenes_) {
        if (!slot.alive) continue;
        for (auto it = slot.scene.objects_.rbegin(); it != slot.scene.objects_.rend(); ++it) {
            RuntimeSceneError ignored;
            (void)destroy_body(*it, ignored);
            (void)destroy_renderer(*it, ignored);
        }
    }
}

namespace {

void emit_progress(
    const RuntimeSceneProgressCallback& callback,
    RuntimeSceneLoadPhase phase,
    std::size_t completed,
    std::size_t total,
    std::optional<std::uint64_t> objectId = std::nullopt) noexcept {
    if (!callback) return;
    try {
        callback(RuntimeSceneProgress{phase, completed, total, objectId});
    } catch (...) {
        // Progress reporting is observational and cannot invalidate deterministic staging.
    }
}

[[nodiscard]] RuntimeSceneError cancelled_error(
    const std::filesystem::path& path,
    RuntimeSceneLoadPhase phase,
    std::optional<std::uint64_t> objectId = std::nullopt) {
    return {
        RuntimeSceneErrorCode::Cancelled,
        std::string("runtime scene staging cancelled during phase ") +
            std::to_string(static_cast<unsigned>(phase)),
        path,
        objectId};
}

[[nodiscard]] SolverBox voxel_box_to_solver_box(
    const VoxelBox& box,
    float voxelSizeMeters) noexcept {
    const Float3 minimum{
        static_cast<float>(box.min.x) * voxelSizeMeters,
        static_cast<float>(box.min.y) * voxelSizeMeters,
        static_cast<float>(box.min.z) * voxelSizeMeters};
    const Float3 maximum{
        static_cast<float>(box.maxExclusive.x) * voxelSizeMeters,
        static_cast<float>(box.maxExclusive.y) * voxelSizeMeters,
        static_cast<float>(box.maxExclusive.z) * voxelSizeMeters};
    return {
        multiply(add(minimum, maximum), 0.5F),
        multiply(subtract(maximum, minimum), 0.5F)};
}

constexpr std::array<char, 8> kCheckpointMagic{'D', 'V', 'E', 'C', 'K', 'P', '0', '1'};
constexpr std::uint32_t kCheckpointVersion = 1U;

struct CheckpointObjectState {
    std::uint64_t id{};
    std::uint64_t metadataHash{};
    bool loaded{};
    bool hasBody{};
    std::uint64_t authorityHash{};
    std::uint64_t assetContentHash{};
    RigidBodyState bodyState{};
};

struct CheckpointState {
    std::vector<CheckpointObjectState> objects;
};

[[nodiscard]] std::uint64_t hash_object_metadata(
    const RuntimeSceneObjectMetadata& metadata) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_value(hash, metadata.index);
    hash_value(hash, metadata.id);
    hash_string(hash, metadata.name);
    hash_string(hash, metadata.nodePath);
    const std::uint64_t parent = metadata.parentId.value_or(0U);
    hash_value(hash, parent);
    hash_value(hash, metadata.anchored);
    hash_value(hash, metadata.structural);
    hash_value(hash, metadata.generateCollision);
    hash_transform(hash, metadata.worldTransform);
    return hash;
}


[[nodiscard]] std::uint64_t hash_voxel_occupancy(const VoxelObject& object) noexcept {
    std::uint64_t hash = kFnvOffset;
    std::uint64_t occupiedBricks = 0U;
    for (const BrickKey key : object.bricks().sorted_keys()) {
        const Brick* brick = object.find_brick(key);
        if (brick == nullptr) continue;
        const Bitset512 occupancy = brick->occupancy();
        if (occupancy.none()) continue;
        ++occupiedBricks;
        hash_value(hash, key.x);
        hash_value(hash, key.y);
        hash_value(hash, key.z);
        for (const std::uint64_t word : occupancy.words) hash_value(hash, word);
    }
    hash_value(hash, occupiedBricks);
    return hash;
}

void append_u8(std::vector<std::uint8_t>& output, std::uint8_t value) {
    output.push_back(value);
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void append_u64(std::vector<std::uint8_t>& output, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64U; shift += 8U) {
        output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
}

void append_float(std::vector<std::uint8_t>& output, float value) {
    append_u32(output, std::bit_cast<std::uint32_t>(value));
}

void append_transform(std::vector<std::uint8_t>& output, const RigidTransform& transform) {
    append_float(output, transform.position.x);
    append_float(output, transform.position.y);
    append_float(output, transform.position.z);
    append_float(output, transform.rotation.x);
    append_float(output, transform.rotation.y);
    append_float(output, transform.rotation.z);
    append_float(output, transform.rotation.w);
}

void append_state(std::vector<std::uint8_t>& output, const RigidBodyState& state) {
    append_transform(output, state.previousTransform);
    append_transform(output, state.currentTransform);
    append_float(output, state.linearVelocity.x);
    append_float(output, state.linearVelocity.y);
    append_float(output, state.linearVelocity.z);
    append_float(output, state.angularVelocity.x);
    append_float(output, state.angularVelocity.y);
    append_float(output, state.angularVelocity.z);
    append_u8(output, state.sleeping ? 1U : 0U);
    for (int i = 0; i < 7; ++i) append_u8(output, 0U);
}

[[nodiscard]] std::uint64_t checkpoint_checksum(
    std::span<const std::uint8_t> bytes) noexcept {
    std::uint64_t hash = kFnvOffset;
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= kFnvPrime;
    }
    return hash;
}

[[nodiscard]] bool write_checkpoint_state_file(
    const std::filesystem::path& path,
    const CheckpointState& state,
    std::string& error) {
    if (state.objects.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "checkpoint object count exceeds format limit";
        return false;
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(32U + state.objects.size() * 160U);
    bytes.insert(bytes.end(), kCheckpointMagic.begin(), kCheckpointMagic.end());
    append_u32(bytes, kCheckpointVersion);
    append_u32(bytes, static_cast<std::uint32_t>(state.objects.size()));
    for (const CheckpointObjectState& object : state.objects) {
        append_u64(bytes, object.id);
        append_u64(bytes, object.metadataHash);
        append_u8(bytes, object.loaded ? 1U : 0U);
        append_u8(bytes, object.hasBody ? 1U : 0U);
        for (int i = 0; i < 6; ++i) append_u8(bytes, 0U);
        append_u64(bytes, object.authorityHash);
        append_u64(bytes, object.assetContentHash);
        append_state(bytes, object.bodyState);
    }
    append_u64(bytes, checkpoint_checksum(bytes));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "unable to create checkpoint state file";
        return false;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        error = "unable to write checkpoint state file";
        return false;
    }
    return true;
}

class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool read_u8(std::uint8_t& value) {
        if (position_ >= bytes_.size()) return false;
        value = bytes_[position_++];
        return true;
    }
    [[nodiscard]] bool read_u32(std::uint32_t& value) {
        if (bytes_.size() - position_ < 4U) return false;
        value = 0U;
        for (unsigned shift = 0; shift < 32U; shift += 8U) {
            value |= static_cast<std::uint32_t>(bytes_[position_++]) << shift;
        }
        return true;
    }
    [[nodiscard]] bool read_u64(std::uint64_t& value) {
        if (bytes_.size() - position_ < 8U) return false;
        value = 0U;
        for (unsigned shift = 0; shift < 64U; shift += 8U) {
            value |= static_cast<std::uint64_t>(bytes_[position_++]) << shift;
        }
        return true;
    }
    [[nodiscard]] bool read_float(float& value) {
        std::uint32_t bits{};
        if (!read_u32(bits)) return false;
        value = std::bit_cast<float>(bits);
        return true;
    }
    [[nodiscard]] bool read_transform(RigidTransform& transform) {
        return read_float(transform.position.x) && read_float(transform.position.y) &&
            read_float(transform.position.z) && read_float(transform.rotation.x) &&
            read_float(transform.rotation.y) && read_float(transform.rotation.z) &&
            read_float(transform.rotation.w);
    }
    [[nodiscard]] bool read_state(RigidBodyState& state) {
        std::uint8_t sleeping{};
        if (!read_transform(state.previousTransform) || !read_transform(state.currentTransform) ||
            !read_float(state.linearVelocity.x) || !read_float(state.linearVelocity.y) ||
            !read_float(state.linearVelocity.z) || !read_float(state.angularVelocity.x) ||
            !read_float(state.angularVelocity.y) || !read_float(state.angularVelocity.z) ||
            !read_u8(sleeping)) return false;
        state.sleeping = sleeping != 0U;
        std::uint8_t ignored{};
        for (int i = 0; i < 7; ++i) if (!read_u8(ignored)) return false;
        return true;
    }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_{};
};

[[nodiscard]] bool read_checkpoint_state_file(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes,
    CheckpointState& state,
    std::string& error) {
    std::error_code filesystemError;
    const std::uint64_t fileSize = std::filesystem::file_size(path, filesystemError);
    if (filesystemError || fileSize > maximumBytes || fileSize < 24U ||
        fileSize > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        error = "checkpoint state file size is invalid";
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open checkpoint state file";
        return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) {
        error = "unable to read checkpoint state file";
        return false;
    }
    if (!std::equal(kCheckpointMagic.begin(), kCheckpointMagic.end(), bytes.begin())) {
        error = "checkpoint state magic is invalid";
        return false;
    }
    std::uint64_t storedChecksum{};
    ByteReader checksumReader(std::span<const std::uint8_t>(bytes).subspan(bytes.size() - 8U));
    if (!checksumReader.read_u64(storedChecksum) ||
        checkpoint_checksum(std::span<const std::uint8_t>(bytes).first(bytes.size() - 8U)) != storedChecksum) {
        error = "checkpoint state checksum mismatch";
        return false;
    }

    ByteReader reader(std::span<const std::uint8_t>(bytes).first(bytes.size() - 8U));
    std::uint8_t ignored{};
    for (std::size_t i = 0; i < kCheckpointMagic.size(); ++i) {
        if (!reader.read_u8(ignored)) return false;
    }
    std::uint32_t version{};
    std::uint32_t count{};
    if (!reader.read_u32(version) || version != kCheckpointVersion || !reader.read_u32(count)) {
        error = "checkpoint state version is unsupported";
        return false;
    }
    state.objects.clear();
    state.objects.reserve(count);
    std::unordered_set<std::uint64_t> ids;
    ids.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        CheckpointObjectState object;
        std::uint8_t loaded{};
        std::uint8_t hasBody{};
        if (!reader.read_u64(object.id) || !reader.read_u64(object.metadataHash) ||
            !reader.read_u8(loaded) || !reader.read_u8(hasBody)) {
            error = "checkpoint state object record is truncated";
            return false;
        }
        for (int padding = 0; padding < 6; ++padding) {
            if (!reader.read_u8(ignored)) {
                error = "checkpoint state object padding is truncated";
                return false;
            }
        }
        if (!reader.read_u64(object.authorityHash) || !reader.read_u64(object.assetContentHash) ||
            !reader.read_state(object.bodyState)) {
            error = "checkpoint state object payload is truncated";
            return false;
        }
        object.loaded = loaded != 0U;
        object.hasBody = hasBody != 0U;
        if (object.id == 0U || !ids.insert(object.id).second ||
            (object.hasBody && !object.loaded) ||
            (object.hasBody && !rigid_body_state_is_finite(object.bodyState))) {
            error = "checkpoint state object record is invalid";
            return false;
        }
        state.objects.push_back(std::move(object));
    }
    if (reader.position() != bytes.size() - 8U) {
        error = "checkpoint state contains trailing bytes";
        return false;
    }
    return true;
}

[[nodiscard]] std::string json_escape(std::string_view input) {
    std::string result;
    result.reserve(input.size() + 8U);
    for (const unsigned char character : input) {
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20U) {
                constexpr char digits[] = "0123456789abcdef";
                result += "\\u00";
                result.push_back(digits[(character >> 4U) & 0xFU]);
                result.push_back(digits[character & 0xFU]);
            } else {
                result.push_back(static_cast<char>(character));
            }
        }
    }
    return result;
}

[[nodiscard]] std::array<float, 16> transform_matrix_column_major(
    const RigidTransform& transform) noexcept {
    const Quaternion q = normalize(transform.rotation);
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float xz = q.x * q.z;
    const float yz = q.y * q.z;
    const float wx = q.w * q.x;
    const float wy = q.w * q.y;
    const float wz = q.w * q.z;
    const float r00 = 1.0F - 2.0F * (yy + zz);
    const float r01 = 2.0F * (xy - wz);
    const float r02 = 2.0F * (xz + wy);
    const float r10 = 2.0F * (xy + wz);
    const float r11 = 1.0F - 2.0F * (xx + zz);
    const float r12 = 2.0F * (yz - wx);
    const float r20 = 2.0F * (xz - wy);
    const float r21 = 2.0F * (yz + wx);
    const float r22 = 1.0F - 2.0F * (xx + yy);
    return {
        r00, r10, r20, 0.0F,
        r01, r11, r21, 0.0F,
        r02, r12, r22, 0.0F,
        transform.position.x, transform.position.y, transform.position.z, 1.0F};
}

[[nodiscard]] bool write_checkpoint_manifest(
    const std::filesystem::path& path,
    const RuntimeScene& scene,
    const std::vector<std::string>& filenames,
    std::string& error) {
    if (filenames.size() != scene.objects().size()) {
        error = "checkpoint filename count mismatch";
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = "unable to create checkpoint scene manifest";
        return false;
    }
    output << std::setprecision(std::numeric_limits<float>::max_digits10);
    output << "{\n  \"format\":\"DVOXSCENE\",\"version\":1,\"name\":\""
           << json_escape(scene.name()) << "\",\n  \"objects\":[\n";
    for (std::size_t i = 0; i < scene.objects().size(); ++i) {
        const RuntimeSceneObjectMetadata& metadata = scene.objects()[i].metadata();
        const std::array<float, 16> matrix = transform_matrix_column_major(metadata.worldTransform);
        output << "    {\"index\":" << metadata.index
               << ",\"id\":\"" << metadata.id << "\""
               << ",\"name\":\"" << json_escape(metadata.name) << "\""
               << ",\"nodePath\":\"" << json_escape(metadata.nodePath) << "\""
               << ",\"file\":\"" << json_escape(filenames[i]) << "\""
               << ",\"parent\":";
        if (metadata.parentIndex) output << *metadata.parentIndex;
        else output << "null";
        output << ",\"anchored\":" << (metadata.anchored ? "true" : "false")
               << ",\"structural\":" << (metadata.structural ? "true" : "false")
               << ",\"generateCollision\":" << (metadata.generateCollision ? "true" : "false")
               << ",\"worldMatrix\":[";
        for (std::size_t element = 0; element < matrix.size(); ++element) {
            if (element != 0U) output << ',';
            output << matrix[element];
        }
        output << "]}" << (i + 1U == scene.objects().size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
    if (!output) {
        error = "unable to write checkpoint scene manifest";
        return false;
    }
    return true;
}

} // namespace

bool RuntimeSceneWorld::load_object_asset(
    RuntimeSceneObject& object,
    const RuntimeSceneLoadOptions& options,
    RuntimeSceneError& error) {
    try {
        std::error_code pathError;
        const std::filesystem::path currentCanonical =
            std::filesystem::weakly_canonical(object.resolvedAssetPath_, pathError);
        if (pathError || currentCanonical != object.resolvedAssetPath_) {
            fail(RuntimeSceneErrorCode::PathEscape,
                 "DVOX asset path changed after manifest validation",
                 object.resolvedAssetPath_, object.metadata_.id);
        }
        if (!std::filesystem::is_regular_file(currentCanonical, pathError) || pathError) {
            fail(RuntimeSceneErrorCode::MissingAsset,
                 "DVOX asset is no longer a regular file",
                 object.resolvedAssetPath_, object.metadata_.id);
        }
        DvoxReadResult read = read_dvox(currentCanonical, options.maximumDvoxBytesPerObject);
        if (!read.success) {
            fail(RuntimeSceneErrorCode::DvoxReadFailed,
                 "DVOX load failed: " + read.error, object.resolvedAssetPath_, object.metadata_.id);
        }
        if (read.asset.object.id() != object.metadata_.id) {
            fail(RuntimeSceneErrorCode::ObjectIdMismatch,
                 "DVOX object ID does not match the scene manifest",
                 object.resolvedAssetPath_, object.metadata_.id);
        }
        for (const VoxelMaterialDefinition& material : read.asset.materials) {
            if (!validate_material(material)) {
                fail(RuntimeSceneErrorCode::InvalidMaterialTable,
                     "DVOX material table contains non-finite or invalid values",
                     object.resolvedAssetPath_, object.metadata_.id);
            }
        }
        if (!validate_material_references(read.asset)) {
            fail(RuntimeSceneErrorCode::InvalidMaterialReference,
                 "DVOX voxels reference a material outside the material table",
                 object.resolvedAssetPath_, object.metadata_.id);
        }
        const float voxelSize = read.asset.voxelSizeMeters;
        if (options.expectedVoxelSizeMeters > 0.0F &&
            std::abs(voxelSize - options.expectedVoxelSizeMeters) > options.voxelSizeTolerance) {
            fail(RuntimeSceneErrorCode::VoxelSizeMismatch,
                 "DVOX voxel size violates the external scene policy",
                 object.resolvedAssetPath_, object.metadata_.id);
        }
        object.asset_.emplace(std::move(read.asset));
        return true;
    } catch (const SceneException& exception) {
        error = exception.error;
    } catch (const std::exception& exception) {
        error = {RuntimeSceneErrorCode::DvoxReadFailed, exception.what(),
                 object.resolvedAssetPath_, object.metadata_.id};
    }
    object.asset_.reset();
    return false;
}

bool RuntimeSceneWorld::derive_object(
    RuntimeSceneObject& object,
    const RuntimeSceneLoadOptions& options,
    RuntimeSceneError& error) {
    try {
        if (!object.asset_) {
            fail(RuntimeSceneErrorCode::DerivedDataFailure,
                 "derived-data construction requires a loaded asset",
                 object.resolvedAssetPath_, object.metadata_.id);
        }
        object.packedBrickmap_.emplace();
        object.packedBrickmap_->reserve(
            object.asset_->object.brick_count(), object.asset_->object.payload_bytes());
        object.packedBrickmap_->rebuild(object.asset_->object);
        if (!object.packedBrickmap_->validate_against(object.asset_->object)) {
            fail(RuntimeSceneErrorCode::DerivedDataFailure,
                 "packed brickmap did not validate against the authoritative object",
                 object.resolvedAssetPath_, object.metadata_.id);
        }

        object.connectivity_.emplace();
        const std::size_t bricks = object.asset_->object.brick_count();
        object.connectivity_->reserve(
            bricks, std::max<std::size_t>(bricks, 1U) * 2U,
            std::max<std::size_t>(bricks, 1U) * 6U);
        object.connectivity_->initialize_with_anchor_masks(
            object.asset_->object,
            [&](BrickKey key) {
                if (!object.metadata_.anchored) return Bitset512{};
                const Brick* brick = object.asset_->object.find_brick(key);
                return brick == nullptr ? Bitset512{} : brick->occupancy();
            });
        if (!object.connectivity_->validate(object.asset_->object)) {
            fail(RuntimeSceneErrorCode::DerivedDataFailure,
                 "connectivity cache did not validate against the authoritative object",
                 object.resolvedAssetPath_, object.metadata_.id);
        }

        if (object.metadata_.generateCollision) {
            object.collisionBoxes_ = build_merged_object_box_proxy(object.asset_->object);
            object.collisionProxyOverBudget_ = options.maximumProxyBoxes != 0U &&
                                               object.collisionBoxes_.size() > options.maximumProxyBoxes;
            if (object.collisionProxyOverBudget_ && options.rejectProxyOverBudget) {
                fail(RuntimeSceneErrorCode::DerivedDataFailure,
                     "collision proxy exceeds the configured box budget",
                     object.resolvedAssetPath_, object.metadata_.id);
            }

            if (object.metadata_.anchored && !object.collisionBoxes_.empty()) {
                StaticRigidBodyCreateDesc staticDesc;
                staticDesc.transform = object.metadata_.worldTransform;
                staticDesc.collisionClass = RigidBodyCollisionClass::Full;
                staticDesc.boxes.reserve(object.collisionBoxes_.size());
                for (const VoxelBox& box : object.collisionBoxes_) {
                    staticDesc.boxes.push_back(
                        voxel_box_to_solver_box(box, object.asset_->voxelSizeMeters));
                }
                if (!validate_static_rigid_body_desc(staticDesc)) {
                    fail(RuntimeSceneErrorCode::DerivedDataFailure,
                         "static rigid-body descriptor failed validation",
                         object.resolvedAssetPath_, object.metadata_.id);
                }
                object.initialStaticBodyDesc_ = std::move(staticDesc);
            } else if (!object.metadata_.anchored && !object.collisionBoxes_.empty()) {
                double densityQuantum = 0.0;
                const MaterialMassTable massTable = make_material_mass_table(*object.asset_, densityQuantum);
                const float voxelSizeMeters = object.asset_->voxelSizeMeters;
                const RigidTransform voxelUnitTransform = make_rigid_transform(
                    multiply(object.metadata_.worldTransform.position, 1.0F / voxelSizeMeters),
                    object.metadata_.worldTransform.rotation);
                const FragmentSolverPackage package = build_fragment_solver_package(
                    object.asset_->object, voxelUnitTransform, massTable, options.maximumProxyBoxes);
                const FragmentSolverScale scale{
                    densityQuantum * static_cast<double>(voxelSizeMeters) * voxelSizeMeters * voxelSizeMeters,
                    voxelSizeMeters};
                std::optional<RigidBodyCreateDesc> desc = make_rigid_body_desc(package, scale);
                if (!desc) {
                    fail(RuntimeSceneErrorCode::DerivedDataFailure,
                         "unable to construct a valid dynamic rigid-body descriptor",
                         object.resolvedAssetPath_, object.metadata_.id);
                }
                desc->collisionClass = object.metadata_.structural
                    ? RigidBodyCollisionClass::Full
                    : RigidBodyCollisionClass::DebrisNoSelf;
                if (!validate_rigid_body_desc(*desc)) {
                    fail(RuntimeSceneErrorCode::DerivedDataFailure,
                         "dynamic rigid-body descriptor failed final meter-space validation",
                         object.resolvedAssetPath_, object.metadata_.id);
                }
                object.initialBodyDesc_ = std::move(desc);
            }
        }
        return true;
    } catch (const SceneException& exception) {
        error = exception.error;
    } catch (const std::exception& exception) {
        error = {RuntimeSceneErrorCode::DerivedDataFailure, exception.what(),
                 object.resolvedAssetPath_, object.metadata_.id};
    }
    object.packedBrickmap_.reset();
    object.connectivity_.reset();
    object.collisionBoxes_.clear();
    object.initialBodyDesc_.reset();
    object.initialStaticBodyDesc_.reset();
    object.collisionProxyOverBudget_ = false;
    return false;
}

std::size_t RuntimeSceneWorld::estimate_staging_bytes(const RuntimeScene& sceneValue) noexcept {
    std::size_t total = 0U;
    const auto add_bytes = [&total](std::size_t bytes) noexcept {
        if (bytes > std::numeric_limits<std::size_t>::max() - total) {
            total = std::numeric_limits<std::size_t>::max();
        } else {
            total += bytes;
        }
    };
    const auto multiply_bytes = [](std::size_t a, std::size_t b) noexcept {
        if (a != 0U && b > std::numeric_limits<std::size_t>::max() / a) {
            return std::numeric_limits<std::size_t>::max();
        }
        return a * b;
    };

    add_bytes(sizeof(RuntimeScene));
    add_bytes(multiply_bytes(sceneValue.objects_.capacity(), sizeof(RuntimeSceneObject)));
    for (const RuntimeSceneObject& object : sceneValue.objects_) {
        add_bytes(object.metadata_.name.capacity());
        add_bytes(object.metadata_.nodePath.capacity());
        if (!object.asset_) continue;
        add_bytes(object.asset_->object.logical_storage_bytes());
        add_bytes(multiply_bytes(object.asset_->materials.capacity(), sizeof(VoxelMaterialDefinition)));
        for (const VoxelMaterialDefinition& material : object.asset_->materials) {
            add_bytes(material.name.capacity());
            add_bytes(multiply_bytes(material.layers.capacity(), sizeof(VoxelMaterialLayer)));
        }
        if (object.packedBrickmap_) {
            const PackedBrickmapStorageStats stats = object.packedBrickmap_->storage_stats();
            add_bytes(stats.recordBytes);
            add_bytes(multiply_bytes(stats.indexCells, sizeof(std::uint32_t)));
            add_bytes(stats.materialArenaBytes);
        }
        if (object.connectivity_) add_bytes(object.connectivity_->storage_stats().totalCapacityBytes);
        add_bytes(multiply_bytes(object.collisionBoxes_.capacity(), sizeof(VoxelBox)));
        if (object.initialBodyDesc_) {
            add_bytes(sizeof(RigidBodyCreateDesc));
            add_bytes(multiply_bytes(object.initialBodyDesc_->boxes.capacity(), sizeof(SolverBox)));
        }
        if (object.initialStaticBodyDesc_) {
            add_bytes(sizeof(StaticRigidBodyCreateDesc));
            add_bytes(multiply_bytes(object.initialStaticBodyDesc_->boxes.capacity(), sizeof(SolverBox)));
        }
    }
    return total;
}

RuntimeSceneStageResult RuntimeSceneWorld::stage_scene_package(
    const std::filesystem::path& manifestPath,
    const RuntimeSceneLoadOptions& options,
    const RuntimeSceneStageOptions& stageOptions) const {
    const std::size_t workerCount = stageOptions.workerCount == 0U
        ? std::min<std::size_t>(JobSystem::default_worker_count(), 8U)
        : std::min<std::size_t>(stageOptions.workerCount, 64U);
    JobSystem jobs(workerCount);
    return stage_scene_package_with_jobs(manifestPath, options, stageOptions, jobs);
}

RuntimeSceneStageResult RuntimeSceneWorld::stage_scene_package_with_jobs(
    const std::filesystem::path& manifestPath,
    const RuntimeSceneLoadOptions& options,
    const RuntimeSceneStageOptions& stageOptions,
    JobSystem& jobs) const {
    RuntimeSceneStageResult result;
    try {
        if (options.maximumObjects == 0U || options.maximumManifestBytes == 0U ||
            options.maximumDvoxBytesPerObject == 0U || options.voxelSizeTolerance < 0.0F ||
            !std::isfinite(options.voxelSizeTolerance) || options.expectedVoxelSizeMeters < 0.0F ||
            !std::isfinite(options.expectedVoxelSizeMeters) || stageOptions.maximumStagingBytes == 0U) {
            fail(RuntimeSceneErrorCode::InvalidManifest,
                 "runtime scene staging options are invalid", manifestPath);
        }
        if (stageOptions.cancellation.cancellation_requested()) {
            result.error = cancelled_error(manifestPath, RuntimeSceneLoadPhase::Manifest);
            return result;
        }
        emit_progress(stageOptions.progress, RuntimeSceneLoadPhase::Manifest, 0U, 1U);
        RuntimeScene staged = RuntimeSceneParserAccess::parse_manifest(manifestPath, options);
        emit_progress(stageOptions.progress, RuntimeSceneLoadPhase::Manifest, 1U, 1U);
        if (stageOptions.cancellation.cancellation_requested()) {
            result.error = cancelled_error(staged.manifestPath_, RuntimeSceneLoadPhase::Manifest);
            return result;
        }

        std::unordered_set<std::uint64_t> requested;
        requested.reserve(options.initiallyLoadedObjectIds.size());
        for (const std::uint64_t id : options.initiallyLoadedObjectIds) {
            if (!requested.insert(id).second) continue;
            if (staged.find_object(id) == nullptr) {
                fail(RuntimeSceneErrorCode::UnknownRequestedObject,
                     "initiallyLoadedObjectIds contains an ID absent from the manifest",
                     staged.manifestPath_, id);
            }
        }
        std::vector<std::size_t> selected;
        selected.reserve(staged.objects_.size());
        for (std::size_t i = 0; i < staged.objects_.size(); ++i) {
            if (options.loadAllObjects || requested.contains(staged.objects_[i].metadata_.id)) {
                selected.push_back(i);
            }
        }

        std::vector<RuntimeSceneError> objectErrors(staged.objects_.size());

        emit_progress(stageOptions.progress, RuntimeSceneLoadPhase::AssetIo, 0U, selected.size());
        jobs.parallel_for(selected.size(), [&](std::size_t selectedIndex) {
            if (stageOptions.cancellation.cancellation_requested()) return;
            const std::size_t objectIndex = selected[selectedIndex];
            RuntimeSceneError error;
            if (!load_object_asset(staged.objects_[objectIndex], options, error)) {
                objectErrors[objectIndex] = std::move(error);
            }
        });
        if (stageOptions.cancellation.cancellation_requested()) {
            result.error = cancelled_error(staged.manifestPath_, RuntimeSceneLoadPhase::AssetIo);
            return result;
        }
        for (const std::size_t objectIndex : selected) {
            if (objectErrors[objectIndex]) {
                result.error = std::move(objectErrors[objectIndex]);
                return result;
            }
        }
        emit_progress(stageOptions.progress, RuntimeSceneLoadPhase::AssetIo,
                      selected.size(), selected.size());

        float uniformVoxelSize = options.expectedVoxelSizeMeters > 0.0F
            ? options.expectedVoxelSizeMeters : 0.0F;
        bool mixedVoxelSizes = false;
        for (const std::size_t objectIndex : selected) {
            const RuntimeSceneObject& object = staged.objects_[objectIndex];
            const float voxelSize = object.asset_->voxelSizeMeters;
            if (!mixedVoxelSizes) {
                if (uniformVoxelSize == 0.0F) {
                    uniformVoxelSize = voxelSize;
                } else if (std::abs(voxelSize - uniformVoxelSize) > options.voxelSizeTolerance) {
                    if (options.requireUniformVoxelSize) {
                        fail(RuntimeSceneErrorCode::VoxelSizeMismatch,
                             "scene package contains inconsistent voxel sizes",
                             object.resolvedAssetPath_, object.metadata_.id);
                    }
                    uniformVoxelSize = 0.0F;
                    mixedVoxelSizes = true;
                }
            }
        }
        staged.uniformVoxelSizeMeters_ = uniformVoxelSize;
        staged.mixedVoxelSizes_ = mixedVoxelSizes;

        std::size_t estimatedBytes = estimate_staging_bytes(staged);
        if (estimatedBytes > stageOptions.maximumStagingBytes) {
            fail(RuntimeSceneErrorCode::StagingMemoryExceeded,
                 "loaded scene assets exceed the configured staging-memory limit",
                 staged.manifestPath_);
        }

        std::fill(objectErrors.begin(), objectErrors.end(), RuntimeSceneError{});
        emit_progress(stageOptions.progress, RuntimeSceneLoadPhase::DerivedData, 0U, selected.size());
        jobs.parallel_for(selected.size(), [&](std::size_t selectedIndex) {
            if (stageOptions.cancellation.cancellation_requested()) return;
            const std::size_t objectIndex = selected[selectedIndex];
            RuntimeSceneError error;
            if (!derive_object(staged.objects_[objectIndex], options, error)) {
                objectErrors[objectIndex] = std::move(error);
            }
        });
        if (stageOptions.cancellation.cancellation_requested()) {
            result.error = cancelled_error(staged.manifestPath_, RuntimeSceneLoadPhase::DerivedData);
            return result;
        }
        for (const std::size_t objectIndex : selected) {
            if (objectErrors[objectIndex]) {
                result.error = std::move(objectErrors[objectIndex]);
                return result;
            }
        }
        emit_progress(stageOptions.progress, RuntimeSceneLoadPhase::DerivedData,
                      selected.size(), selected.size());

        estimatedBytes = estimate_staging_bytes(staged);
        if (estimatedBytes > stageOptions.maximumStagingBytes) {
            fail(RuntimeSceneErrorCode::StagingMemoryExceeded,
                 "derived scene caches exceed the configured staging-memory limit",
                 staged.manifestPath_);
        }

        result.staging.scene_.emplace(std::move(staged));
        result.staging.estimatedBytes_ = estimatedBytes;
        result.staging.cancellation_ = stageOptions.cancellation;
        result.staging.progress_ = stageOptions.progress;
        return result;
    } catch (const SceneException& exception) {
        result.error = exception.error;
    } catch (const std::exception& exception) {
        result.error = {RuntimeSceneErrorCode::InvalidManifest, exception.what(), manifestPath, std::nullopt};
    }
    return result;
}

std::future<RuntimeSceneStageResult> RuntimeSceneWorld::stage_scene_package_async(
    std::filesystem::path manifestPath,
    RuntimeSceneLoadOptions options,
    RuntimeSceneStageOptions stageOptions) const {
    return std::async(
        std::launch::async,
        [manifestPath = std::move(manifestPath), options = std::move(options),
         stageOptions = std::move(stageOptions)]() mutable {
            RuntimeSceneWorld stagingOnly;
            return stagingOnly.stage_scene_package(manifestPath, options, stageOptions);
        });
}

bool RuntimeSceneWorld::publish_renderer_batch(
    RuntimeScene& sceneValue,
    RuntimeSceneError& error) {
    if (rendererWorld_ == nullptr) return true;
    std::vector<RuntimeSceneObject*> objects;
    std::vector<RuntimeBrickmapCreateDesc> descs;
    for (RuntimeSceneObject& object : sceneValue.objects_) {
        if (!object.loaded() || !object.packedBrickmap_) continue;
        objects.push_back(&object);
        descs.push_back({object.metadata_.id, object.metadata_.worldTransform,
                         &*object.packedBrickmap_});
    }
    if (descs.empty()) return true;
    std::vector<RuntimeBrickmapHandle> handles = rendererWorld_->create_objects(descs);
    if (handles.size() != descs.size() ||
        std::any_of(handles.begin(), handles.end(), [](RuntimeBrickmapHandle handle) {
            return handle == kInvalidRuntimeBrickmapHandle;
        })) {
        if (!handles.empty()) (void)rendererWorld_->destroy_objects(handles);
        error = {RuntimeSceneErrorCode::RendererPublicationFailure,
                 "renderer backend rejected the staged scene batch",
                 sceneValue.manifestPath_, std::nullopt};
        return false;
    }
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const std::optional<std::uint64_t> readback = rendererWorld_->readback_hash(handles[i]);
        const std::uint64_t expected = objects[i]->packedBrickmap_->readback_hash();
        if (!readback || *readback != expected) {
            (void)rendererWorld_->destroy_objects(handles);
            error = {RuntimeSceneErrorCode::RendererPublicationFailure,
                     "renderer readback hash differs from the staged packed brickmap",
                     objects[i]->resolvedAssetPath_, objects[i]->metadata_.id};
            return false;
        }
    }
    for (std::size_t i = 0; i < objects.size(); ++i) {
        objects[i]->rendererHandle_ = handles[i];
        objects[i]->rendererReadbackHash_ = objects[i]->packedBrickmap_->readback_hash();
    }
    return true;
}

bool RuntimeSceneWorld::publish_physics_batch(
    RuntimeScene& sceneValue,
    RuntimeSceneError& error) {
    if (physicsWorld_ == nullptr) return true;

    std::vector<RuntimeSceneObject*> staticObjects;
    std::vector<StaticRigidBodyCreateDesc> staticDescs;
    std::vector<RuntimeSceneObject*> dynamicObjects;
    std::vector<RigidBodyCreateDesc> dynamicDescs;
    for (RuntimeSceneObject& object : sceneValue.objects_) {
        if (!object.loaded()) continue;
        if (sceneValue.options_.createStaticBodies && object.initialStaticBodyDesc_) {
            staticObjects.push_back(&object);
            staticDescs.push_back(*object.initialStaticBodyDesc_);
        } else if (sceneValue.options_.createDynamicBodies && object.initialBodyDesc_) {
            dynamicObjects.push_back(&object);
            dynamicDescs.push_back(*object.initialBodyDesc_);
        }
    }

    std::vector<RigidBodyHandle> staticHandles;
    if (!staticDescs.empty()) {
        staticHandles = physicsWorld_->create_static_bodies(staticDescs);
        if (staticHandles.size() != staticDescs.size()) {
            if (!staticHandles.empty()) (void)physicsWorld_->destroy_bodies(staticHandles);
            error = {RuntimeSceneErrorCode::PhysicsPublicationFailure,
                     "rigid-body backend rejected the static scene batch",
                     sceneValue.manifestPath_, std::nullopt};
            return false;
        }
    }

    std::vector<RigidBodyHandle> dynamicHandles;
    if (!dynamicDescs.empty()) {
        dynamicHandles = physicsWorld_->create_bodies(dynamicDescs);
        if (dynamicHandles.size() != dynamicDescs.size()) {
            if (!dynamicHandles.empty()) (void)physicsWorld_->destroy_bodies(dynamicHandles);
            if (!staticHandles.empty()) (void)physicsWorld_->destroy_bodies(staticHandles);
            error = {RuntimeSceneErrorCode::PhysicsPublicationFailure,
                     "rigid-body backend rejected the dynamic scene batch",
                     sceneValue.manifestPath_, std::nullopt};
            return false;
        }
    }

    for (std::size_t i = 0; i < staticObjects.size(); ++i) {
        staticObjects[i]->bodyHandle_ = staticHandles[i];
    }
    for (std::size_t i = 0; i < dynamicObjects.size(); ++i) {
        dynamicObjects[i]->bodyHandle_ = dynamicHandles[i];
    }
    return true;
}

bool RuntimeSceneWorld::publish_renderer(
    RuntimeSceneObject& object,
    RuntimeSceneError& error) {
    if (rendererWorld_ == nullptr || !object.packedBrickmap_) return true;
    const RuntimeBrickmapCreateDesc desc{
        object.metadata_.id, object.metadata_.worldTransform, &*object.packedBrickmap_};
    const RuntimeBrickmapHandle handle = rendererWorld_->create_object(desc);
    if (handle == kInvalidRuntimeBrickmapHandle) {
        error = {RuntimeSceneErrorCode::RendererPublicationFailure,
                 "renderer backend rejected the staged object",
                 object.resolvedAssetPath_, object.metadata_.id};
        return false;
    }
    const std::optional<std::uint64_t> readback = rendererWorld_->readback_hash(handle);
    const std::uint64_t expected = object.packedBrickmap_->readback_hash();
    if (!readback || *readback != expected) {
        (void)rendererWorld_->destroy_object(handle);
        error = {RuntimeSceneErrorCode::RendererPublicationFailure,
                 "renderer readback hash differs from the staged packed brickmap",
                 object.resolvedAssetPath_, object.metadata_.id};
        return false;
    }
    object.rendererHandle_ = handle;
    object.rendererReadbackHash_ = expected;
    return true;
}

bool RuntimeSceneWorld::publish_body(
    RuntimeSceneObject& object,
    const RuntimeSceneLoadOptions& options,
    RuntimeSceneError& error) {
    if (physicsWorld_ == nullptr) return true;
    RigidBodyHandle handle = kInvalidRigidBodyHandle;
    if (options.createStaticBodies && object.initialStaticBodyDesc_) {
        handle = physicsWorld_->create_static_body(*object.initialStaticBodyDesc_);
    } else if (options.createDynamicBodies && object.initialBodyDesc_) {
        handle = physicsWorld_->create_body(*object.initialBodyDesc_);
    } else {
        return true;
    }
    if (handle == kInvalidRigidBodyHandle) {
        error = {RuntimeSceneErrorCode::PhysicsPublicationFailure,
                 "rigid-body backend rejected the staged body",
                 object.resolvedAssetPath_, object.metadata_.id};
        return false;
    }
    object.bodyHandle_ = handle;
    return true;
}

bool RuntimeSceneWorld::destroy_renderer(
    RuntimeSceneObject& object,
    RuntimeSceneError& error) {
    if (object.rendererHandle_ == kInvalidRuntimeBrickmapHandle) return true;
    if (rendererWorld_ == nullptr || !rendererWorld_->destroy_object(object.rendererHandle_)) {
        error = {RuntimeSceneErrorCode::RendererPublicationFailure,
                 "renderer backend failed to destroy a scene object",
                 object.resolvedAssetPath_, object.metadata_.id};
        return false;
    }
    object.rendererHandle_ = kInvalidRuntimeBrickmapHandle;
    object.rendererReadbackHash_ = 0U;
    return true;
}

bool RuntimeSceneWorld::destroy_body(RuntimeSceneObject& object, RuntimeSceneError& error) {
    if (object.bodyHandle_ == kInvalidRigidBodyHandle) return true;
    if (physicsWorld_ == nullptr || !physicsWorld_->destroy_body(object.bodyHandle_)) {
        error = {RuntimeSceneErrorCode::PhysicsPublicationFailure,
                 "rigid-body backend failed to destroy a scene body",
                 object.resolvedAssetPath_, object.metadata_.id};
        return false;
    }
    object.bodyHandle_ = kInvalidRigidBodyHandle;
    return true;
}

RuntimeSceneLoadResult RuntimeSceneWorld::publish_staged_scene(
    RuntimeSceneStaging&& staging) {
    RuntimeSceneLoadResult result;
    if (!on_authority_thread()) {
        result.error = {RuntimeSceneErrorCode::WrongThread,
                        "scene publication must run on the RuntimeSceneWorld authority thread",
                        {}, std::nullopt};
        return result;
    }
    if (!staging.scene_) {
        result.error = {RuntimeSceneErrorCode::InvalidStaging,
                        "runtime scene staging object is empty", {}, std::nullopt};
        return result;
    }

    RuntimeScene staged = std::move(*staging.scene_);
    staging.scene_.reset();
    staging.memoryReservation_.reset();
    try {
        if (staging.cancellation_.cancellation_requested()) {
            result.error = cancelled_error(staged.manifestPath_, RuntimeSceneLoadPhase::RendererPublication);
            return result;
        }
        for (const RuntimeSceneObject& object : staged.objects_) {
            if (object_id_reserved(object.metadata_.id)) {
                fail(RuntimeSceneErrorCode::ObjectIdAlreadyReserved,
                     "object ID is already reserved by a loaded scene",
                     staged.manifestPath_, object.metadata_.id);
            }
        }

        emit_progress(staging.progress_, RuntimeSceneLoadPhase::RendererPublication, 0U, 1U);
        if (staging.cancellation_.cancellation_requested()) {
            result.error = cancelled_error(staged.manifestPath_, RuntimeSceneLoadPhase::RendererPublication);
            return result;
        }
        RuntimeSceneError publicationError;
        if (!publish_renderer_batch(staged, publicationError)) {
            throw SceneException(publicationError.code, publicationError.message,
                                 publicationError.path, publicationError.objectId);
        }
        emit_progress(staging.progress_, RuntimeSceneLoadPhase::RendererPublication, 1U, 1U);

        if (staging.cancellation_.cancellation_requested()) {
            for (auto it = staged.objects_.rbegin(); it != staged.objects_.rend(); ++it) {
                RuntimeSceneError ignored;
                (void)destroy_renderer(*it, ignored);
            }
            result.error = cancelled_error(staged.manifestPath_, RuntimeSceneLoadPhase::PhysicsPublication);
            return result;
        }
        emit_progress(staging.progress_, RuntimeSceneLoadPhase::PhysicsPublication, 0U, 1U);
        if (staging.cancellation_.cancellation_requested()) {
            for (auto it = staged.objects_.rbegin(); it != staged.objects_.rend(); ++it) {
                RuntimeSceneError ignored;
                (void)destroy_renderer(*it, ignored);
            }
            result.error = cancelled_error(staged.manifestPath_, RuntimeSceneLoadPhase::PhysicsPublication);
            return result;
        }
        if (!publish_physics_batch(staged, publicationError)) {
            for (auto it = staged.objects_.rbegin(); it != staged.objects_.rend(); ++it) {
                RuntimeSceneError ignored;
                (void)destroy_renderer(*it, ignored);
            }
            throw SceneException(publicationError.code, publicationError.message,
                                 publicationError.path, publicationError.objectId);
        }
        emit_progress(staging.progress_, RuntimeSceneLoadPhase::PhysicsPublication, 1U, 1U);

        if (staging.cancellation_.cancellation_requested()) {
            for (auto it = staged.objects_.rbegin(); it != staged.objects_.rend(); ++it) {
                RuntimeSceneError ignored;
                (void)destroy_body(*it, ignored);
                (void)destroy_renderer(*it, ignored);
            }
            result.error = cancelled_error(staged.manifestPath_, RuntimeSceneLoadPhase::Commit);
            return result;
        }
        emit_progress(staging.progress_, RuntimeSceneLoadPhase::Commit, 0U, 1U);
        if (staging.cancellation_.cancellation_requested()) {
            for (auto it = staged.objects_.rbegin(); it != staged.objects_.rend(); ++it) {
                RuntimeSceneError ignored;
                (void)destroy_body(*it, ignored);
                (void)destroy_renderer(*it, ignored);
            }
            result.error = cancelled_error(staged.manifestPath_, RuntimeSceneLoadPhase::Commit);
            return result;
        }
        result.handle = commit_scene(std::move(staged));
        emit_progress(staging.progress_, RuntimeSceneLoadPhase::Commit, 1U, 1U);
        emit_progress(staging.progress_, RuntimeSceneLoadPhase::Complete, 1U, 1U);
        return result;
    } catch (const SceneException& exception) {
        for (auto it = staged.objects_.rbegin(); it != staged.objects_.rend(); ++it) {
            RuntimeSceneError ignored;
            (void)destroy_body(*it, ignored);
            (void)destroy_renderer(*it, ignored);
        }
        result.error = exception.error;
    } catch (const std::exception& exception) {
        for (auto it = staged.objects_.rbegin(); it != staged.objects_.rend(); ++it) {
            RuntimeSceneError ignored;
            (void)destroy_body(*it, ignored);
            (void)destroy_renderer(*it, ignored);
        }
        result.error = {RuntimeSceneErrorCode::InvalidStaging, exception.what(),
                        staged.manifestPath_, std::nullopt};
    }
    return result;
}

RuntimeSceneLoadResult RuntimeSceneWorld::load_scene_package(
    const std::filesystem::path& manifestPath,
    const RuntimeSceneLoadOptions& options) {
    RuntimeSceneStageResult staged = stage_scene_package(manifestPath, options);
    if (!staged) return {kInvalidRuntimeSceneHandle, std::move(staged.error)};
    return publish_staged_scene(std::move(staged.staging));
}

std::optional<RuntimeSceneWorld::DeferredStageSnapshot> RuntimeSceneWorld::snapshot_deferred_object(
    RuntimeSceneHandle sceneHandle,
    std::uint64_t objectId,
    RuntimeSceneError& error) const {
    if (!on_authority_thread()) {
        error = {RuntimeSceneErrorCode::WrongThread,
                 "deferred staging must be requested on the authority thread", {}, objectId};
        return std::nullopt;
    }
    const RuntimeScene* loadedScene = scene(sceneHandle);
    if (loadedScene == nullptr) {
        error = {RuntimeSceneErrorCode::InvalidHandle,
                 "runtime scene handle is invalid", {}, objectId};
        return std::nullopt;
    }
    const RuntimeSceneObject* target = loadedScene->find_object(objectId);
    if (target == nullptr) {
        error = {RuntimeSceneErrorCode::UnknownRequestedObject,
                 "object ID is absent from the scene", loadedScene->manifestPath_, objectId};
        return std::nullopt;
    }
    if (target->loaded()) {
        error = {RuntimeSceneErrorCode::InvalidStaging,
                 "object is already resident", target->resolvedAssetPath_, objectId};
        return std::nullopt;
    }
    DeferredStageSnapshot snapshot;
    snapshot.sceneHandle = sceneHandle;
    snapshot.objectId = objectId;
    snapshot.residencyGeneration = target->residencyGeneration_;
    snapshot.options = loadedScene->options_;
    snapshot.metadata = target->metadata_;
    snapshot.resolvedAssetPath = target->resolvedAssetPath_;
    return snapshot;
}

RuntimeDeferredObjectStageResult RuntimeSceneWorld::stage_deferred_snapshot(
    DeferredStageSnapshot snapshot,
    const RuntimeSceneStageOptions& stageOptions) {
    RuntimeDeferredObjectStageResult result;
    try {
        if (stageOptions.maximumStagingBytes == 0U) {
            fail(RuntimeSceneErrorCode::InvalidManifest,
                 "deferred staging options are invalid", snapshot.resolvedAssetPath,
                 snapshot.objectId);
        }
        if (stageOptions.cancellation.cancellation_requested()) {
            result.error = cancelled_error(snapshot.resolvedAssetPath,
                                           RuntimeSceneLoadPhase::AssetIo,
                                           snapshot.objectId);
            return result;
        }

        RuntimeScene stagedScene;
        stagedScene.options_ = snapshot.options;
        stagedScene.manifestPath_ = snapshot.resolvedAssetPath;
        RuntimeSceneObject object;
        object.metadata_ = std::move(snapshot.metadata);
        object.resolvedAssetPath_ = std::move(snapshot.resolvedAssetPath);
        object.residencyGeneration_ = snapshot.residencyGeneration;
        stagedScene.objects_.push_back(std::move(object));

        emit_progress(stageOptions.progress, RuntimeSceneLoadPhase::AssetIo, 0U, 1U,
                      snapshot.objectId);
        RuntimeSceneError localError;
        if (!load_object_asset(stagedScene.objects_.front(), snapshot.options, localError)) {
            result.error = std::move(localError);
            return result;
        }
        emit_progress(stageOptions.progress, RuntimeSceneLoadPhase::AssetIo, 1U, 1U,
                      snapshot.objectId);
        if (stageOptions.cancellation.cancellation_requested()) {
            result.error = cancelled_error(stagedScene.objects_.front().resolvedAssetPath_,
                                           RuntimeSceneLoadPhase::AssetIo,
                                           snapshot.objectId);
            return result;
        }
        std::size_t estimatedBytes = estimate_staging_bytes(stagedScene);
        if (estimatedBytes > stageOptions.maximumStagingBytes) {
            fail(RuntimeSceneErrorCode::StagingMemoryExceeded,
                 "deferred asset exceeds the configured staging-memory limit",
                 stagedScene.objects_.front().resolvedAssetPath_, snapshot.objectId);
        }

        emit_progress(stageOptions.progress, RuntimeSceneLoadPhase::DerivedData, 0U, 1U,
                      snapshot.objectId);
        if (!derive_object(stagedScene.objects_.front(), snapshot.options, localError)) {
            result.error = std::move(localError);
            return result;
        }
        emit_progress(stageOptions.progress, RuntimeSceneLoadPhase::DerivedData, 1U, 1U,
                      snapshot.objectId);
        if (stageOptions.cancellation.cancellation_requested()) {
            result.error = cancelled_error(stagedScene.objects_.front().resolvedAssetPath_,
                                           RuntimeSceneLoadPhase::DerivedData,
                                           snapshot.objectId);
            return result;
        }
        estimatedBytes = estimate_staging_bytes(stagedScene);
        if (estimatedBytes > stageOptions.maximumStagingBytes) {
            fail(RuntimeSceneErrorCode::StagingMemoryExceeded,
                 "deferred derived data exceeds the configured staging-memory limit",
                 stagedScene.objects_.front().resolvedAssetPath_, snapshot.objectId);
        }

        result.staging.sceneHandle_ = snapshot.sceneHandle;
        result.staging.objectId_ = snapshot.objectId;
        result.staging.expectedResidencyGeneration_ = snapshot.residencyGeneration;
        result.staging.options_ = snapshot.options;
        result.staging.object_.emplace(std::move(stagedScene.objects_.front()));
        result.staging.estimatedBytes_ = estimatedBytes;
        result.staging.cancellation_ = stageOptions.cancellation;
        result.staging.progress_ = stageOptions.progress;
        return result;
    } catch (const SceneException& exception) {
        result.error = exception.error;
    } catch (const std::exception& exception) {
        result.error = {RuntimeSceneErrorCode::DerivedDataFailure, exception.what(),
                        snapshot.resolvedAssetPath, snapshot.objectId};
    }
    return result;
}

RuntimeDeferredObjectStageResult RuntimeSceneWorld::stage_deferred_object(
    RuntimeSceneHandle sceneHandle,
    std::uint64_t objectId,
    const RuntimeSceneStageOptions& stageOptions) const {
    RuntimeSceneError error;
    std::optional<DeferredStageSnapshot> snapshot =
        snapshot_deferred_object(sceneHandle, objectId, error);
    if (!snapshot) return {RuntimeDeferredObjectStaging{}, std::move(error)};
    return stage_deferred_snapshot(std::move(*snapshot), stageOptions);
}

std::future<RuntimeDeferredObjectStageResult> RuntimeSceneWorld::stage_deferred_object_async(
    RuntimeSceneHandle sceneHandle,
    std::uint64_t objectId,
    RuntimeSceneStageOptions stageOptions) const {
    RuntimeSceneError error;
    std::optional<DeferredStageSnapshot> snapshot =
        snapshot_deferred_object(sceneHandle, objectId, error);
    if (!snapshot) {
        std::promise<RuntimeDeferredObjectStageResult> promise;
        promise.set_value({RuntimeDeferredObjectStaging{}, std::move(error)});
        return promise.get_future();
    }
    return std::async(std::launch::async,
        [snapshot = std::move(*snapshot), stageOptions = std::move(stageOptions)]() mutable {
            return RuntimeSceneWorld::stage_deferred_snapshot(std::move(snapshot), stageOptions);
        });
}

bool RuntimeSceneWorld::publish_staged_deferred_object(
    RuntimeDeferredObjectStaging&& staging,
    RuntimeSceneError* error) {
    RuntimeSceneError localError;
    if (!on_authority_thread()) {
        localError = {RuntimeSceneErrorCode::WrongThread,
                      "deferred publication must run on the authority thread", {},
                      staging.objectId_ == 0U ? std::nullopt : std::optional<std::uint64_t>(staging.objectId_)};
        if (error) *error = localError;
        return false;
    }
    if (!staging.object_) {
        localError = {RuntimeSceneErrorCode::InvalidStaging,
                      "deferred staging object is empty", {}, staging.objectId_};
        if (error) *error = localError;
        return false;
    }
    if (staging.cancellation_.cancellation_requested()) {
        localError = cancelled_error(staging.object_->resolvedAssetPath_,
                                     RuntimeSceneLoadPhase::RendererPublication,
                                     staging.objectId_);
        if (error) *error = localError;
        return false;
    }
    RuntimeScene* loadedScene = scene(staging.sceneHandle_);
    if (loadedScene == nullptr) {
        localError = {RuntimeSceneErrorCode::InvalidHandle,
                      "runtime scene handle became invalid before deferred publication", {},
                      staging.objectId_};
        if (error) *error = localError;
        return false;
    }
    RuntimeSceneObject* target = nullptr;
    for (RuntimeSceneObject& candidate : loadedScene->objects_) {
        if (candidate.metadata_.id == staging.objectId_) { target = &candidate; break; }
    }
    if (target == nullptr) {
        localError = {RuntimeSceneErrorCode::UnknownRequestedObject,
                      "object ID disappeared before deferred publication",
                      loadedScene->manifestPath_, staging.objectId_};
        if (error) *error = localError;
        return false;
    }
    if (target->loaded() || target->residencyGeneration_ != staging.expectedResidencyGeneration_ ||
        target->resolvedAssetPath_ != staging.object_->resolvedAssetPath_) {
        localError = {RuntimeSceneErrorCode::InvalidStaging,
                      "deferred staging result is stale or duplicates an existing publication",
                      target->resolvedAssetPath_, staging.objectId_};
        if (error) *error = localError;
        return false;
    }

    RuntimeSceneObject staged = std::move(*staging.object_);
    staging.object_.reset();
    staging.memoryReservation_.reset();
    float uniformVoxelSize = loadedScene->uniformVoxelSizeMeters_;
    bool mixedVoxelSizes = loadedScene->mixedVoxelSizes_;
    const float voxelSize = staged.asset_->voxelSizeMeters;
    if (!mixedVoxelSizes) {
        if (uniformVoxelSize == 0.0F) uniformVoxelSize = voxelSize;
        else if (std::abs(voxelSize - uniformVoxelSize) > loadedScene->options_.voxelSizeTolerance) {
            if (loadedScene->options_.requireUniformVoxelSize) {
                localError = {RuntimeSceneErrorCode::VoxelSizeMismatch,
                              "deferred object violates the scene voxel-size policy",
                              staged.resolvedAssetPath_, staging.objectId_};
                if (error) *error = localError;
                return false;
            }
            uniformVoxelSize = 0.0F;
            mixedVoxelSizes = true;
        }
    }

    emit_progress(staging.progress_, RuntimeSceneLoadPhase::RendererPublication, 0U, 1U,
                  staging.objectId_);
    if (!publish_renderer(staged, localError)) {
        if (error) *error = localError;
        return false;
    }
    emit_progress(staging.progress_, RuntimeSceneLoadPhase::RendererPublication, 1U, 1U,
                  staging.objectId_);
    if (staging.cancellation_.cancellation_requested()) {
        RuntimeSceneError ignored;
        (void)destroy_renderer(staged, ignored);
        localError = cancelled_error(staged.resolvedAssetPath_,
                                     RuntimeSceneLoadPhase::PhysicsPublication,
                                     staging.objectId_);
        if (error) *error = localError;
        return false;
    }

    emit_progress(staging.progress_, RuntimeSceneLoadPhase::PhysicsPublication, 0U, 1U,
                  staging.objectId_);
    if (!publish_body(staged, loadedScene->options_, localError)) {
        RuntimeSceneError ignored;
        (void)destroy_renderer(staged, ignored);
        if (error) *error = localError;
        return false;
    }
    emit_progress(staging.progress_, RuntimeSceneLoadPhase::PhysicsPublication, 1U, 1U,
                  staging.objectId_);

    staged.residencyGeneration_ = target->residencyGeneration_ + 1U;
    *target = std::move(staged);
    loadedScene->uniformVoxelSizeMeters_ = uniformVoxelSize;
    loadedScene->mixedVoxelSizes_ = mixedVoxelSizes;
    emit_progress(staging.progress_, RuntimeSceneLoadPhase::Complete, 1U, 1U,
                  staging.objectId_);
    return true;
}

bool RuntimeSceneWorld::load_deferred_object(
    RuntimeSceneHandle sceneHandle,
    std::uint64_t objectId,
    RuntimeSceneError* error) {
    const RuntimeScene* loadedScene = scene(sceneHandle);
    if (loadedScene != nullptr) {
        const RuntimeSceneObject* existing = loadedScene->find_object(objectId);
        if (existing != nullptr && existing->loaded()) return true;
    }
    RuntimeDeferredObjectStageResult staged = stage_deferred_object(sceneHandle, objectId);
    if (!staged) {
        if (error) *error = std::move(staged.error);
        return false;
    }
    return publish_staged_deferred_object(std::move(staged.staging), error);
}

bool RuntimeSceneWorld::unload_object(
    RuntimeSceneHandle sceneHandle,
    std::uint64_t objectId,
    RuntimeSceneError* error) {
    RuntimeSceneError localError;
    if (!on_authority_thread()) {
        localError = {RuntimeSceneErrorCode::WrongThread,
                      "scene unload must run on the authority thread", {}, objectId};
        if (error) *error = localError;
        return false;
    }
    RuntimeScene* loadedScene = scene(sceneHandle);
    if (loadedScene == nullptr) {
        localError = {RuntimeSceneErrorCode::InvalidHandle,
                      "runtime scene handle is invalid", {}, objectId};
        if (error) *error = localError;
        return false;
    }
    RuntimeSceneObject* object = nullptr;
    for (RuntimeSceneObject& candidate : loadedScene->objects_) {
        if (candidate.metadata_.id == objectId) { object = &candidate; break; }
    }
    if (object == nullptr) {
        localError = {RuntimeSceneErrorCode::UnknownRequestedObject,
                      "object ID is absent from the scene", loadedScene->manifestPath_, objectId};
        if (error) *error = localError;
        return false;
    }
    if (!object->loaded()) return true;
    if (!destroy_body(*object, localError) || !destroy_renderer(*object, localError)) {
        if (error) *error = localError;
        return false;
    }
    object->asset_.reset();
    object->packedBrickmap_.reset();
    object->connectivity_.reset();
    object->collisionBoxes_.clear();
    object->initialBodyDesc_.reset();
    object->initialStaticBodyDesc_.reset();
    object->collisionProxyOverBudget_ = false;
    ++object->residencyGeneration_;
    return true;
}

bool RuntimeSceneWorld::unload_scene(
    RuntimeSceneHandle sceneHandle,
    RuntimeSceneError* error) {
    if (!on_authority_thread()) {
        if (error) *error = {RuntimeSceneErrorCode::WrongThread,
                             "scene unload must run on the authority thread", {}, std::nullopt};
        return false;
    }
    RuntimeScene* loadedScene = scene(sceneHandle);
    if (loadedScene == nullptr) {
        if (error) *error = {RuntimeSceneErrorCode::InvalidHandle,
                             "runtime scene handle is invalid", {}, std::nullopt};
        return false;
    }
    for (auto it = loadedScene->objects_.rbegin(); it != loadedScene->objects_.rend(); ++it) {
        RuntimeSceneError localError;
        if (!destroy_body(*it, localError) || !destroy_renderer(*it, localError)) {
            if (error) *error = localError;
            return false;
        }
    }
    SceneSlot& slot = scenes_[sceneHandle];
    slot.scene = RuntimeScene{};
    slot.alive = false;
    freeHandles_.push_back(sceneHandle);
    return true;
}

bool RuntimeSceneWorld::save_scene_checkpoint(
    RuntimeSceneHandle sceneHandle,
    const std::filesystem::path& checkpointDirectory,
    RuntimeSceneError* error) const {
    RuntimeSceneError localError;
    if (!on_authority_thread()) {
        localError = {RuntimeSceneErrorCode::WrongThread,
                      "scene checkpoint capture must run on the authority thread",
                      checkpointDirectory, std::nullopt};
        if (error) *error = localError;
        return false;
    }
    const RuntimeScene* loadedScene = scene(sceneHandle);
    if (loadedScene == nullptr) {
        localError = {RuntimeSceneErrorCode::InvalidHandle,
                      "runtime scene handle is invalid", checkpointDirectory, std::nullopt};
        if (error) *error = localError;
        return false;
    }
    std::error_code filesystemError;
    if (std::filesystem::exists(checkpointDirectory, filesystemError)) {
        localError = {RuntimeSceneErrorCode::SavedStateIo,
                      "checkpoint destination already exists", checkpointDirectory, std::nullopt};
        if (error) *error = localError;
        return false;
    }
    const std::filesystem::path parent = checkpointDirectory.parent_path().empty()
        ? std::filesystem::current_path()
        : checkpointDirectory.parent_path();
    std::filesystem::create_directories(parent, filesystemError);
    if (filesystemError) {
        localError = {RuntimeSceneErrorCode::SavedStateIo,
                      "unable to create checkpoint parent directory", parent, std::nullopt};
        if (error) *error = localError;
        return false;
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path temporary = parent /
        (checkpointDirectory.filename().string() + ".tmp_" + std::to_string(stamp));
    std::filesystem::create_directories(temporary, filesystemError);
    if (filesystemError) {
        localError = {RuntimeSceneErrorCode::SavedStateIo,
                      "unable to create temporary checkpoint directory", temporary, std::nullopt};
        if (error) *error = localError;
        return false;
    }

    const auto cleanup = [&]() noexcept {
        std::error_code ignored;
        std::filesystem::remove_all(temporary, ignored);
    };
    try {
        CheckpointState state;
        state.objects.reserve(loadedScene->objects_.size());
        std::vector<std::string> filenames;
        filenames.reserve(loadedScene->objects_.size());
        for (const RuntimeSceneObject& object : loadedScene->objects_) {
            const std::string filename = "object_" + std::to_string(object.metadata_.id) + ".dvox";
            filenames.push_back(filename);
            const std::filesystem::path outputPath = temporary / filename;
            if (object.loaded()) {
                std::string writeError;
                if (!write_dvox(outputPath, *object.asset_, {}, &writeError)) {
                    throw SceneException(RuntimeSceneErrorCode::SavedStateIo,
                                         "unable to write checkpoint DVOX: " + writeError,
                                         outputPath, object.metadata_.id);
                }
            } else {
                std::error_code copyError;
                std::filesystem::copy_file(
                    object.resolvedAssetPath_, outputPath,
                    std::filesystem::copy_options::overwrite_existing, copyError);
                if (copyError) {
                    throw SceneException(RuntimeSceneErrorCode::SavedStateIo,
                                         "unable to copy deferred DVOX into checkpoint",
                                         object.resolvedAssetPath_, object.metadata_.id);
                }
            }
            const std::uint64_t bytes = std::filesystem::file_size(outputPath, filesystemError);
            if (filesystemError || bytes > loadedScene->options_.maximumDvoxBytesPerObject) {
                throw SceneException(RuntimeSceneErrorCode::SavedStateIo,
                                     "checkpoint DVOX exceeds the configured object-size limit",
                                     outputPath, object.metadata_.id);
            }

            CheckpointObjectState saved;
            saved.id = object.metadata_.id;
            saved.metadataHash = hash_object_metadata(object.metadata_);
            saved.loaded = object.loaded();
            if (object.loaded()) {
                const RuntimeSceneObjectStats stats = object.stats();
                saved.authorityHash = stats.authorityHash;
                saved.assetContentHash = stats.assetContentHash;
            }
            if (object.bodyHandle_ != kInvalidRigidBodyHandle) {
                if (physicsWorld_ == nullptr) {
                    throw SceneException(RuntimeSceneErrorCode::StateRestoreFailure,
                                         "scene body exists without a physics capability",
                                         checkpointDirectory, object.metadata_.id);
                }
                const std::optional<RigidBodyState> bodyState =
                    physicsWorld_->state(object.bodyHandle_);
                if (!bodyState || !rigid_body_state_is_finite(*bodyState)) {
                    throw SceneException(RuntimeSceneErrorCode::StateRestoreFailure,
                                         "unable to capture a valid rigid-body state",
                                         checkpointDirectory, object.metadata_.id);
                }
                saved.hasBody = true;
                saved.bodyState = *bodyState;
            }
            state.objects.push_back(std::move(saved));
        }

        std::string writeError;
        if (!write_checkpoint_manifest(
                temporary / "checkpoint.dvoxscene.json", *loadedScene, filenames, writeError)) {
            throw SceneException(RuntimeSceneErrorCode::SavedStateIo, writeError,
                                 temporary / "checkpoint.dvoxscene.json");
        }
        if (!write_checkpoint_state_file(temporary / "checkpoint.dvstate", state, writeError)) {
            throw SceneException(RuntimeSceneErrorCode::SavedStateIo, writeError,
                                 temporary / "checkpoint.dvstate");
        }
        std::filesystem::rename(temporary, checkpointDirectory, filesystemError);
        if (filesystemError) {
            throw SceneException(RuntimeSceneErrorCode::SavedStateIo,
                                 "unable to atomically publish checkpoint directory",
                                 checkpointDirectory);
        }
        return true;
    } catch (const SceneException& exception) {
        cleanup();
        localError = exception.error;
    } catch (const std::exception& exception) {
        cleanup();
        localError = {RuntimeSceneErrorCode::SavedStateIo, exception.what(),
                      checkpointDirectory, std::nullopt};
    }
    if (error) *error = localError;
    return false;
}

RuntimeSceneLoadResult RuntimeSceneWorld::restore_scene_checkpoint(
    const std::filesystem::path& checkpointDirectory,
    const RuntimeSceneCheckpointOptions& options) {
    RuntimeSceneLoadResult result;
    if (!on_authority_thread()) {
        result.error = {RuntimeSceneErrorCode::WrongThread,
                        "scene checkpoint restore must run on the authority thread",
                        checkpointDirectory, std::nullopt};
        return result;
    }
    CheckpointState state;
    std::string stateError;
    if (!read_checkpoint_state_file(
            checkpointDirectory / "checkpoint.dvstate",
            options.maximumStateBytes,
            state,
            stateError)) {
        result.error = {RuntimeSceneErrorCode::SavedStateInvalid, stateError,
                        checkpointDirectory / "checkpoint.dvstate", std::nullopt};
        return result;
    }

    RuntimeSceneLoadOptions loadOptions = options.loadOptions;
    loadOptions.loadAllObjects = false;
    loadOptions.initiallyLoadedObjectIds.clear();
    for (const CheckpointObjectState& object : state.objects) {
        if (object.loaded) loadOptions.initiallyLoadedObjectIds.push_back(object.id);
    }
    RuntimeSceneStageResult staged = stage_scene_package(
        checkpointDirectory / "checkpoint.dvoxscene.json",
        loadOptions,
        options.stageOptions);
    if (!staged) return {kInvalidRuntimeSceneHandle, std::move(staged.error)};

    RuntimeScene& stagedScene = *staged.staging.scene_;
    if (stagedScene.objects_.size() != state.objects.size()) {
        return {kInvalidRuntimeSceneHandle,
                {RuntimeSceneErrorCode::AssetIncompatible,
                 "checkpoint state and scene manifest object counts differ",
                 checkpointDirectory, std::nullopt}};
    }
    for (const CheckpointObjectState& saved : state.objects) {
        RuntimeSceneObject* object = nullptr;
        for (RuntimeSceneObject& candidate : stagedScene.objects_) {
            if (candidate.metadata_.id == saved.id) { object = &candidate; break; }
        }
        if (object == nullptr || hash_object_metadata(object->metadata_) != saved.metadataHash ||
            object->loaded() != saved.loaded) {
            return {kInvalidRuntimeSceneHandle,
                    {RuntimeSceneErrorCode::AssetIncompatible,
                     "checkpoint object metadata or loaded state is incompatible",
                     checkpointDirectory, saved.id}};
        }
        if (saved.loaded) {
            const RuntimeSceneObjectStats stats = object->stats();
            if (stats.authorityHash != saved.authorityHash ||
                stats.assetContentHash != saved.assetContentHash) {
                return {kInvalidRuntimeSceneHandle,
                        {RuntimeSceneErrorCode::AssetIncompatible,
                         "checkpoint voxel asset hash does not match saved state",
                         object->resolvedAssetPath_, saved.id}};
            }
        }
    }

    result = publish_staged_scene(std::move(staged.staging));
    if (!result) return result;
    if (!options.restoreBodyStates) return result;

    RuntimeScene* restored = scene(result.handle);
    for (const CheckpointObjectState& saved : state.objects) {
        if (!saved.hasBody) continue;
        RuntimeSceneObject* object = nullptr;
        for (RuntimeSceneObject& candidate : restored->objects_) {
            if (candidate.metadata_.id == saved.id) { object = &candidate; break; }
        }
        if (object == nullptr || object->bodyHandle_ == kInvalidRigidBodyHandle ||
            physicsWorld_ == nullptr ||
            !physicsWorld_->set_state(object->bodyHandle_, saved.bodyState)) {
            RuntimeSceneError unloadError;
            (void)unload_scene(result.handle, &unloadError);
            result.handle = kInvalidRuntimeSceneHandle;
            result.error = {RuntimeSceneErrorCode::StateRestoreFailure,
                            "unable to restore rigid-body state after checkpoint publication",
                            checkpointDirectory, saved.id};
            return result;
        }
    }
    return result;
}

RuntimeSceneHotReloadResult RuntimeSceneWorld::hot_reload_scene(
    RuntimeSceneHandle sceneHandle,
    const std::filesystem::path& replacementManifest,
    const RuntimeSceneHotReloadOptions& options) {
    RuntimeSceneHotReloadResult result;
    if (!on_authority_thread()) {
        result.error = {RuntimeSceneErrorCode::WrongThread,
                        "scene hot reload must run on the authority thread",
                        replacementManifest, std::nullopt};
        return result;
    }
    RuntimeScene* oldScene = scene(sceneHandle);
    if (oldScene == nullptr) {
        result.error = {RuntimeSceneErrorCode::InvalidHandle,
                        "runtime scene handle is invalid",
                        replacementManifest, std::nullopt};
        return result;
    }

    RuntimeSceneLoadOptions replacementOptions = oldScene->options_;
    replacementOptions.loadAllObjects = true;
    replacementOptions.initiallyLoadedObjectIds.clear();
    RuntimeSceneStageResult staged = stage_scene_package(
        replacementManifest, replacementOptions, options.stageOptions);
    if (!staged) {
        result.error = std::move(staged.error);
        return result;
    }
    RuntimeScene newScene = std::move(*staged.staging.scene_);
    staged.staging.scene_.reset();

    if (newScene.objects_.size() != oldScene->objects_.size()) {
        result.error = {RuntimeSceneErrorCode::HotReloadIncompatible,
                        "hot reload changes the stable object-ID set",
                        replacementManifest, std::nullopt};
        return result;
    }

    std::unordered_map<std::uint64_t, RuntimeSceneObject*> newById;
    newById.reserve(newScene.objects_.size());
    for (RuntimeSceneObject& object : newScene.objects_) {
        newById.emplace(object.metadata_.id, &object);
    }
    std::unordered_map<std::uint64_t, bool> wasLoaded;
    wasLoaded.reserve(oldScene->objects_.size());
    std::unordered_map<std::uint64_t, RigidBodyState> bodyStates;
    bodyStates.reserve(oldScene->objects_.size());

    for (RuntimeSceneObject& oldObject : oldScene->objects_) {
        const auto found = newById.find(oldObject.metadata_.id);
        if (found == newById.end()) {
            result.error = {RuntimeSceneErrorCode::HotReloadIncompatible,
                            "hot reload removes a reserved object ID",
                            replacementManifest, oldObject.metadata_.id};
            return result;
        }
        RuntimeSceneObject& newObject = *found->second;
        if (oldObject.metadata_.parentId != newObject.metadata_.parentId ||
            oldObject.metadata_.anchored != newObject.metadata_.anchored) {
            result.error = {RuntimeSceneErrorCode::HotReloadIncompatible,
                            "hot reload changes hierarchy or anchored/dynamic classification",
                            replacementManifest, oldObject.metadata_.id};
            return result;
        }

        RuntimeSceneObject oldLoadedAsset;
        const CookedVoxelAsset* oldAsset = oldObject.asset();
        if (oldAsset == nullptr) {
            oldLoadedAsset.metadata_ = oldObject.metadata_;
            oldLoadedAsset.resolvedAssetPath_ = oldObject.resolvedAssetPath_;
            RuntimeSceneError assetError;
            if (!load_object_asset(oldLoadedAsset, oldScene->options_, assetError)) {
                result.error = std::move(assetError);
                return result;
            }
            oldAsset = oldLoadedAsset.asset();
        }
        if (newObject.asset() == nullptr || oldAsset == nullptr ||
            std::abs(newObject.asset()->voxelSizeMeters - oldAsset->voxelSizeMeters) >
                oldScene->options_.voxelSizeTolerance) {
            result.error = {RuntimeSceneErrorCode::HotReloadIncompatible,
                            "hot reload changes an object's voxel size",
                            replacementManifest, oldObject.metadata_.id};
            return result;
        }
        const std::uint64_t oldTopology = hash_voxel_occupancy(oldAsset->object);
        const std::uint64_t newTopology = hash_voxel_occupancy(newObject.asset()->object);
        if (oldTopology != newTopology && !options.allowVoxelTopologyChanges) {
            result.error = {RuntimeSceneErrorCode::HotReloadIncompatible,
                            "hot reload changes voxel occupancy without migration opt-in",
                            replacementManifest, oldObject.metadata_.id};
            return result;
        }
        if (hash_cooked_asset(*oldAsset) != hash_cooked_asset(*newObject.asset())) {
            result.changedObjectIds.push_back(oldObject.metadata_.id);
        }
        wasLoaded.emplace(oldObject.metadata_.id, oldObject.loaded());
        if (options.preserveDynamicBodyState && !oldObject.metadata_.anchored &&
            oldObject.bodyHandle_ != kInvalidRigidBodyHandle && physicsWorld_ != nullptr) {
            const std::optional<RigidBodyState> stateValue =
                physicsWorld_->state(oldObject.bodyHandle_);
            if (!stateValue || !rigid_body_state_is_finite(*stateValue)) {
                result.error = {RuntimeSceneErrorCode::StateRestoreFailure,
                                "unable to capture body state for hot reload",
                                replacementManifest, oldObject.metadata_.id};
                return result;
            }
            bodyStates.emplace(oldObject.metadata_.id, *stateValue);
        }
    }

    // Preserve the old stream residency policy even though all replacement assets were loaded
    // above to validate compatibility and calculate a complete stable-ID diff.
    for (RuntimeSceneObject& object : newScene.objects_) {
        if (wasLoaded.at(object.metadata_.id)) continue;
        object.asset_.reset();
        object.packedBrickmap_.reset();
        object.connectivity_.reset();
        object.collisionBoxes_.clear();
        object.initialBodyDesc_.reset();
        object.initialStaticBodyDesc_.reset();
        object.collisionProxyOverBudget_ = false;
    }
    float uniformVoxelSize = newScene.options_.expectedVoxelSizeMeters > 0.0F
        ? newScene.options_.expectedVoxelSizeMeters : 0.0F;
    bool mixedVoxelSizes = false;
    for (const RuntimeSceneObject& object : newScene.objects_) {
        if (!object.loaded()) continue;
        const float voxelSize = object.asset_->voxelSizeMeters;
        if (!mixedVoxelSizes) {
            if (uniformVoxelSize == 0.0F) uniformVoxelSize = voxelSize;
            else if (std::abs(voxelSize - uniformVoxelSize) > newScene.options_.voxelSizeTolerance) {
                uniformVoxelSize = 0.0F;
                mixedVoxelSizes = true;
            }
        }
    }
    newScene.uniformVoxelSizeMeters_ = uniformVoxelSize;
    newScene.mixedVoxelSizes_ = mixedVoxelSizes;
    newScene.options_.loadAllObjects = false;
    newScene.options_.initiallyLoadedObjectIds.clear();
    for (const RuntimeSceneObject& object : newScene.objects_) {
        if (object.loaded()) newScene.options_.initiallyLoadedObjectIds.push_back(object.metadata_.id);
    }


    // Backends with stable persistent allocations can update renderer data in place. New physics
    // bodies are created first while the old scene remains authoritative. No simulation step
    // occurs during this transaction, so temporary duplicate bodies are invisible to gameplay.
    if (rendererWorld_ != nullptr && rendererWorld_->supports_incremental_update()) {
        struct IncrementalPair {
            RuntimeSceneObject* oldObject{};
            RuntimeSceneObject* newObject{};
        };
        std::vector<IncrementalPair> rendererPairs;
        rendererPairs.reserve(oldScene->objects_.size());
        bool eligible = true;
        for (RuntimeSceneObject& oldObject : oldScene->objects_) {
            RuntimeSceneObject& newObject = *newById.at(oldObject.metadata_.id);
            if (!oldObject.loaded()) {
                if (oldObject.rendererHandle_ != kInvalidRuntimeBrickmapHandle || newObject.loaded()) {
                    eligible = false;
                    break;
                }
                continue;
            }
            if (!newObject.loaded() || !oldObject.packedBrickmap_ || !newObject.packedBrickmap_ ||
                oldObject.rendererHandle_ == kInvalidRuntimeBrickmapHandle) {
                eligible = false;
                break;
            }
            rendererPairs.push_back({&oldObject, &newObject});
        }

        if (eligible) {
            RuntimeSceneError incrementalError;
            if (!publish_physics_batch(newScene, incrementalError)) {
                result.error = std::move(incrementalError);
                return result;
            }

            const auto destroy_new_bodies = [&]() noexcept {
                for (auto it = newScene.objects_.rbegin(); it != newScene.objects_.rend(); ++it) {
                    RuntimeSceneError ignored;
                    (void)destroy_body(*it, ignored);
                }
            };

            if (physicsWorld_ != nullptr && options.preserveDynamicBodyState) {
                for (RuntimeSceneObject& object : newScene.objects_) {
                    const auto stateIt = bodyStates.find(object.metadata_.id);
                    if (stateIt == bodyStates.end()) continue;
                    if (object.bodyHandle_ == kInvalidRigidBodyHandle ||
                        !physicsWorld_->set_state(object.bodyHandle_, stateIt->second)) {
                        destroy_new_bodies();
                        result.error = {RuntimeSceneErrorCode::StateRestoreFailure,
                                        "unable to migrate dynamic body state before incremental renderer publication",
                                        replacementManifest, object.metadata_.id};
                        return result;
                    }
                }
            }

            std::size_t updatedCount = 0U;
            const auto rollback_renderers = [&](std::size_t count) -> bool {
                bool success = true;
                while (count != 0U) {
                    --count;
                    RuntimeSceneObject& oldObject = *rendererPairs[count].oldObject;
                    const RuntimeBrickmapUpdateDesc oldDesc{
                        oldObject.metadata_.id, oldObject.metadata_.worldTransform,
                        &*oldObject.packedBrickmap_};
                    if (!rendererWorld_->update_object(oldObject.rendererHandle_, oldDesc)) {
                        success = false;
                        continue;
                    }
                    const std::optional<std::uint64_t> readback =
                        rendererWorld_->readback_hash(oldObject.rendererHandle_);
                    if (!readback || *readback != oldObject.packedBrickmap_->readback_hash()) {
                        success = false;
                    }
                }
                return success;
            };

            for (IncrementalPair pair : rendererPairs) {
                const RuntimeBrickmapUpdateDesc newDesc{
                    pair.newObject->metadata_.id, pair.newObject->metadata_.worldTransform,
                    &*pair.newObject->packedBrickmap_};
                const bool updated = rendererWorld_->update_object(
                    pair.oldObject->rendererHandle_, newDesc);
                const std::optional<std::uint64_t> readback = updated
                    ? rendererWorld_->readback_hash(pair.oldObject->rendererHandle_)
                    : std::nullopt;
                const std::uint64_t expected = pair.newObject->packedBrickmap_->readback_hash();
                if (!updated || !readback || *readback != expected) {
                    const std::size_t rollbackCount = updated ? updatedCount + 1U : updatedCount;
                    const bool rolledBack = rollback_renderers(rollbackCount);
                    destroy_new_bodies();
                    result.error = rolledBack
                        ? RuntimeSceneError{RuntimeSceneErrorCode::RendererPublicationFailure,
                            "renderer backend rejected an incremental hot-reload update",
                            pair.newObject->resolvedAssetPath_, pair.newObject->metadata_.id}
                        : RuntimeSceneError{RuntimeSceneErrorCode::HotReloadRollbackFailure,
                            "incremental renderer update failed and prior renderer ranges could not be restored",
                            replacementManifest, pair.newObject->metadata_.id};
                    return result;
                }
                pair.newObject->rendererHandle_ = pair.oldObject->rendererHandle_;
                pair.newObject->rendererReadbackHash_ = expected;
                ++updatedCount;
            }

            bool oldBodiesDestroyed = true;
            std::optional<std::uint64_t> failedBodyId;
            for (auto it = oldScene->objects_.rbegin(); it != oldScene->objects_.rend(); ++it) {
                RuntimeSceneError destroyError;
                if (!destroy_body(*it, destroyError)) {
                    oldBodiesDestroyed = false;
                    failedBodyId = it->metadata_.id;
                    break;
                }
            }
            if (!oldBodiesDestroyed) {
                const bool renderersRestored = rollback_renderers(updatedCount);
                destroy_new_bodies();
                // Normalize the partially torn-down old physics publication, then recreate it.
                for (auto it = oldScene->objects_.rbegin(); it != oldScene->objects_.rend(); ++it) {
                    RuntimeSceneError ignored;
                    (void)destroy_body(*it, ignored);
                }
                RuntimeSceneError restoreError;
                bool physicsRestored = publish_physics_batch(*oldScene, restoreError);
                if (physicsRestored && physicsWorld_ != nullptr) {
                    for (RuntimeSceneObject& object : oldScene->objects_) {
                        const auto stateIt = bodyStates.find(object.metadata_.id);
                        if (stateIt != bodyStates.end() &&
                            (object.bodyHandle_ == kInvalidRigidBodyHandle ||
                             !physicsWorld_->set_state(object.bodyHandle_, stateIt->second))) {
                            physicsRestored = false;
                            break;
                        }
                    }
                }
                result.error = renderersRestored && physicsRestored
                    ? RuntimeSceneError{RuntimeSceneErrorCode::PhysicsPublicationFailure,
                        "old body teardown failed during incremental hot reload; old scene was restored",
                        replacementManifest, failedBodyId}
                    : RuntimeSceneError{RuntimeSceneErrorCode::HotReloadRollbackFailure,
                        "old body teardown failed and incremental hot-reload rollback was incomplete",
                        replacementManifest, failedBodyId};
                return result;
            }

            for (RuntimeSceneObject& replacement : newScene.objects_) {
                const RuntimeSceneObject* previous = oldScene->find_object(replacement.metadata_.id);
                if (previous != nullptr) {
                    replacement.residencyGeneration_ = previous->residencyGeneration_ + 1U;
                }
            }
            *oldScene = std::move(newScene);
            return result;
        }
    }

    const auto restore_old_publication = [&]() -> bool {
        RuntimeSceneError rollbackError;
        if (!publish_renderer_batch(*oldScene, rollbackError) ||
            !publish_physics_batch(*oldScene, rollbackError)) return false;
        if (physicsWorld_ != nullptr) {
            for (RuntimeSceneObject& object : oldScene->objects_) {
                const auto stateIt = bodyStates.find(object.metadata_.id);
                if (stateIt != bodyStates.end() &&
                    (object.bodyHandle_ == kInvalidRigidBodyHandle ||
                     !physicsWorld_->set_state(object.bodyHandle_, stateIt->second))) {
                    return false;
                }
            }
        }
        return true;
    };

    // Retain CPU authority while replacing only backend publications. This enables rollback by
    // republishing the old packed scenes and collision descriptors if any new backend rejects.
    for (auto it = oldScene->objects_.rbegin(); it != oldScene->objects_.rend(); ++it) {
        RuntimeSceneError destroyError;
        if (!destroy_body(*it, destroyError) || !destroy_renderer(*it, destroyError)) {
            if (!restore_old_publication()) {
                result.error = {RuntimeSceneErrorCode::HotReloadRollbackFailure,
                                "hot reload teardown failed and old backend publication could not be restored",
                                replacementManifest, it->metadata_.id};
            } else {
                result.error = std::move(destroyError);
            }
            return result;
        }
    }

    RuntimeSceneError publicationError;
    if (!publish_renderer_batch(newScene, publicationError) ||
        !publish_physics_batch(newScene, publicationError)) {
        for (auto it = newScene.objects_.rbegin(); it != newScene.objects_.rend(); ++it) {
            RuntimeSceneError ignored;
            (void)destroy_body(*it, ignored);
            (void)destroy_renderer(*it, ignored);
        }
        if (!restore_old_publication()) {
            result.error = {RuntimeSceneErrorCode::HotReloadRollbackFailure,
                            "replacement publication failed and old scene rollback also failed",
                            replacementManifest, publicationError.objectId};
        } else {
            result.error = std::move(publicationError);
        }
        return result;
    }

    if (physicsWorld_ != nullptr && options.preserveDynamicBodyState) {
        for (RuntimeSceneObject& object : newScene.objects_) {
            const auto stateIt = bodyStates.find(object.metadata_.id);
            if (stateIt == bodyStates.end()) continue;
            if (object.bodyHandle_ == kInvalidRigidBodyHandle ||
                !physicsWorld_->set_state(object.bodyHandle_, stateIt->second)) {
                for (auto it = newScene.objects_.rbegin(); it != newScene.objects_.rend(); ++it) {
                    RuntimeSceneError ignored;
                    (void)destroy_body(*it, ignored);
                    (void)destroy_renderer(*it, ignored);
                }
                if (!restore_old_publication()) {
                    result.error = {RuntimeSceneErrorCode::HotReloadRollbackFailure,
                                    "body-state migration failed and old scene rollback also failed",
                                    replacementManifest, object.metadata_.id};
                } else {
                    result.error = {RuntimeSceneErrorCode::StateRestoreFailure,
                                    "unable to migrate dynamic body state into replacement scene",
                                    replacementManifest, object.metadata_.id};
                }
                return result;
            }
        }
    }

    for (RuntimeSceneObject& replacement : newScene.objects_) {
        const RuntimeSceneObject* previous = oldScene->find_object(replacement.metadata_.id);
        if (previous != nullptr) replacement.residencyGeneration_ = previous->residencyGeneration_ + 1U;
    }
    *oldScene = std::move(newScene);
    return result;
}

const RuntimeScene* RuntimeSceneWorld::scene(RuntimeSceneHandle handle) const noexcept {
    if (handle >= scenes_.size() || !scenes_[handle].alive) return nullptr;
    return &scenes_[handle].scene;
}

RuntimeScene* RuntimeSceneWorld::scene(RuntimeSceneHandle handle) noexcept {
    if (handle >= scenes_.size() || !scenes_[handle].alive) return nullptr;
    return &scenes_[handle].scene;
}

std::size_t RuntimeSceneWorld::scene_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(scenes_.begin(), scenes_.end(),
                                                  [](const SceneSlot& slot) { return slot.alive; }));
}

std::size_t RuntimeSceneWorld::reserved_object_count() const noexcept {
    std::size_t count = 0;
    for (const SceneSlot& slot : scenes_) if (slot.alive) count += slot.scene.objects_.size();
    return count;
}

std::size_t RuntimeSceneWorld::loaded_object_count() const noexcept {
    std::size_t count = 0;
    for (const SceneSlot& slot : scenes_) if (slot.alive) count += slot.scene.loaded_object_count();
    return count;
}

bool RuntimeSceneWorld::object_id_reserved(std::uint64_t objectId) const noexcept {
    for (const SceneSlot& slot : scenes_) {
        if (slot.alive && slot.scene.find_object(objectId) != nullptr) return true;
    }
    return false;
}

std::uint64_t RuntimeSceneWorld::state_hash() const noexcept {
    std::uint64_t hash = kFnvOffset;
    for (std::size_t i = 0; i < scenes_.size(); ++i) {
        if (!scenes_[i].alive) continue;
        hash_value(hash, i);
        const std::uint64_t sceneHash = scenes_[i].scene.state_hash();
        hash_value(hash, sceneHash);
    }
    return hash;
}

RuntimeSceneHandle RuntimeSceneWorld::commit_scene(RuntimeScene&& sceneValue) {
    if (!freeHandles_.empty()) {
        const RuntimeSceneHandle handle = freeHandles_.back();
        freeHandles_.pop_back();
        scenes_[handle].scene = std::move(sceneValue);
        scenes_[handle].alive = true;
        return handle;
    }
    if (scenes_.size() >= static_cast<std::size_t>(kInvalidRuntimeSceneHandle)) {
        fail(RuntimeSceneErrorCode::LimitExceeded, "runtime scene handle space is exhausted");
    }
    SceneSlot slot;
    slot.alive = true;
    slot.scene = std::move(sceneValue);
    scenes_.push_back(std::move(slot));
    return static_cast<RuntimeSceneHandle>(scenes_.size() - 1U);
}


struct RuntimeSceneStagingExecutor::SharedState {
    mutable std::mutex mutex;
    std::condition_variable condition;
    bool stopping{};
    bool preferDeferred{};
    std::deque<std::unique_ptr<Task>> sceneQueue;
    std::deque<std::unique_ptr<Task>> deferredQueue;
    std::size_t maximumRetainedStagingBytes{};
    std::size_t publicationBackpressureBytes{};
    std::size_t maximumPendingJobs{};
    std::size_t retainedStagingBytes{};
    std::size_t peakRetainedStagingBytes{};
    std::size_t runningJobs{};
    std::uint64_t submittedSceneJobs{};
    std::uint64_t submittedDeferredJobs{};
    std::array<std::uint64_t, 4> submittedByPriority{};
    std::uint64_t completedJobs{};
    std::uint64_t failedJobs{};
    std::uint64_t cancelledJobs{};
    std::uint64_t queueRejectedJobs{};
    std::uint64_t deadlineExpiredJobs{};
    std::uint64_t backpressureWaitCount{};
    std::uint64_t totalQueueWaitMicroseconds{};
    std::uint64_t maximumQueueWaitMicroseconds{};
    std::uint64_t totalStageMicroseconds{};
    std::uint64_t maximumStageMicroseconds{};
    std::uint64_t nextSequence{};
    std::optional<RuntimeSceneCancellationToken> runningCancellation;
};

struct RuntimeSceneStagingExecutor::Task {
    enum class Kind : std::uint8_t { Scene, Deferred };
    Kind kind{Kind::Scene};
    RuntimeSceneJobPriority priority{RuntimeSceneJobPriority::Normal};
    std::optional<std::chrono::steady_clock::time_point> deadline{};
    std::chrono::steady_clock::time_point enqueuedAt{};
    std::uint64_t sequence{};
    RuntimeSceneCancellationToken cancellation{};
    std::move_only_function<void(JobSystem&, const std::shared_ptr<SharedState>&)> run;
    std::move_only_function<void(const std::shared_ptr<SharedState>&)> cancel;
    std::move_only_function<void(const std::shared_ptr<SharedState>&)> expire;
};

namespace {

[[nodiscard]] constexpr std::size_t priority_index(RuntimeSceneJobPriority priority) noexcept {
    return static_cast<std::size_t>(priority);
}

struct ExecutorMemoryReservation {
    std::weak_ptr<RuntimeSceneStagingExecutor::SharedState> state;
    std::size_t bytes{};

    ~ExecutorMemoryReservation() {
        if (const auto locked = state.lock()) {
            {
                std::lock_guard lock(locked->mutex);
                locked->retainedStagingBytes = bytes > locked->retainedStagingBytes
                    ? 0U : locked->retainedStagingBytes - bytes;
            }
            locked->condition.notify_all();
        }
    }
};

[[nodiscard]] std::shared_ptr<void> reserve_executor_staging_memory(
    const std::shared_ptr<RuntimeSceneStagingExecutor::SharedState>& state,
    std::size_t bytes) {
    std::lock_guard lock(state->mutex);
    if (bytes > state->maximumRetainedStagingBytes -
                    std::min(state->retainedStagingBytes, state->maximumRetainedStagingBytes)) {
        return {};
    }
    state->retainedStagingBytes += bytes;
    state->peakRetainedStagingBytes =
        std::max(state->peakRetainedStagingBytes, state->retainedStagingBytes);
    return std::shared_ptr<void>(new ExecutorMemoryReservation{state, bytes});
}

void record_executor_result(
    const std::shared_ptr<RuntimeSceneStagingExecutor::SharedState>& state,
    const RuntimeSceneError& error) {
    std::lock_guard lock(state->mutex);
    ++state->completedJobs;
    if (error) {
        ++state->failedJobs;
        if (error.code == RuntimeSceneErrorCode::Cancelled) ++state->cancelledJobs;
        if (error.code == RuntimeSceneErrorCode::DeadlineExceeded) ++state->deadlineExpiredJobs;
    }
}

[[nodiscard]] RuntimeSceneError executor_shutdown_error(
    const std::filesystem::path& path = {},
    std::optional<std::uint64_t> objectId = std::nullopt) {
    return {RuntimeSceneErrorCode::Cancelled,
            "persistent staging executor shut down before the queued job ran",
            path, objectId};
}

[[nodiscard]] RuntimeSceneError executor_deadline_error(
    const std::filesystem::path& path = {},
    std::optional<std::uint64_t> objectId = std::nullopt) {
    return {RuntimeSceneErrorCode::DeadlineExceeded,
            "staging deadline expired before the queued job began",
            path, objectId};
}

[[nodiscard]] RuntimeSceneError executor_queue_full_error(
    const std::filesystem::path& path = {},
    std::optional<std::uint64_t> objectId = std::nullopt) {
    return {RuntimeSceneErrorCode::QueueFull,
            "persistent staging executor pending-job limit reached",
            path, objectId};
}

[[nodiscard]] bool task_precedes(
    const RuntimeSceneStagingExecutor::Task& a,
    const RuntimeSceneStagingExecutor::Task& b) noexcept {
    if (a.priority != b.priority) {
        return priority_index(a.priority) < priority_index(b.priority);
    }
    if (a.deadline && b.deadline && *a.deadline != *b.deadline) return *a.deadline < *b.deadline;
    if (a.deadline.has_value() != b.deadline.has_value()) return a.deadline.has_value();
    return a.sequence < b.sequence;
}

using TaskQueue = std::deque<std::unique_ptr<RuntimeSceneStagingExecutor::Task>>;

[[nodiscard]] TaskQueue::iterator best_task(TaskQueue& queue) {
    return std::min_element(queue.begin(), queue.end(), [](const auto& a, const auto& b) {
        return task_precedes(*a, *b);
    });
}

} // namespace

RuntimeSceneStagingExecutor::RuntimeSceneStagingExecutor(
    std::size_t workerCount,
    std::size_t maximumRetainedStagingBytes)
    : RuntimeSceneStagingExecutor(RuntimeSceneStagingExecutorConfig{
          workerCount, maximumRetainedStagingBytes, 128U, maximumRetainedStagingBytes}) {}

RuntimeSceneStagingExecutor::RuntimeSceneStagingExecutor(
    const RuntimeSceneStagingExecutorConfig& config)
    : state_(std::make_shared<SharedState>()),
      jobs_(config.workerCount == 0U
          ? std::min<std::size_t>(JobSystem::default_worker_count(), 8U)
          : std::min<std::size_t>(config.workerCount, 64U)) {
    if (config.maximumRetainedStagingBytes == 0U) {
        throw std::invalid_argument("maximumRetainedStagingBytes must be positive");
    }
    if (config.maximumPendingJobs == 0U) {
        throw std::invalid_argument("maximumPendingJobs must be positive");
    }
    state_->maximumRetainedStagingBytes = config.maximumRetainedStagingBytes;
    state_->publicationBackpressureBytes = config.publicationBackpressureBytes == 0U
        ? config.maximumRetainedStagingBytes
        : std::min(config.publicationBackpressureBytes, config.maximumRetainedStagingBytes);
    state_->maximumPendingJobs = config.maximumPendingJobs;
    coordinator_ = std::thread([this] { coordinator_loop(); });
}

RuntimeSceneStagingExecutor::~RuntimeSceneStagingExecutor() {
    std::deque<std::unique_ptr<Task>> abandonedScene;
    std::deque<std::unique_ptr<Task>> abandonedDeferred;
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        if (state_->runningCancellation) state_->runningCancellation->request_cancel();
        abandonedScene.swap(state_->sceneQueue);
        abandonedDeferred.swap(state_->deferredQueue);
    }
    for (auto& task : abandonedScene) task->cancel(state_);
    for (auto& task : abandonedDeferred) task->cancel(state_);
    state_->condition.notify_all();
    if (coordinator_.joinable()) coordinator_.join();
}

std::future<RuntimeSceneStageResult> RuntimeSceneStagingExecutor::submit_scene(
    std::filesystem::path manifestPath,
    RuntimeSceneLoadOptions options,
    RuntimeSceneStageOptions stageOptions,
    RuntimeSceneSubmissionOptions submissionOptions) {
    auto promise = std::make_shared<std::promise<RuntimeSceneStageResult>>();
    std::future<RuntimeSceneStageResult> future = promise->get_future();
    const std::filesystem::path cancelPath = manifestPath;
    auto task = std::make_unique<Task>();
    task->kind = Task::Kind::Scene;
    task->priority = submissionOptions.priority;
    task->deadline = submissionOptions.deadline;
    task->enqueuedAt = std::chrono::steady_clock::now();
    task->cancellation = stageOptions.cancellation;
    task->run = [manifestPath = std::move(manifestPath), options = std::move(options),
                 stageOptions = std::move(stageOptions), promise]
        (JobSystem& jobs, const std::shared_ptr<SharedState>& state) mutable {
        RuntimeSceneWorld stagingOnly;
        RuntimeSceneStageResult result = stagingOnly.stage_scene_package_with_jobs(
            manifestPath, options, stageOptions, jobs);
        if (result) {
            result.staging.memoryReservation_ =
                reserve_executor_staging_memory(state, result.staging.estimatedBytes_);
            if (!result.staging.memoryReservation_) {
                result.staging = {};
                result.error = {RuntimeSceneErrorCode::StagingMemoryExceeded,
                                "persistent staging executor retained-memory budget exceeded",
                                manifestPath, std::nullopt};
            }
        }
        record_executor_result(state, result.error);
        promise->set_value(std::move(result));
    };
    task->cancel = [manifestPath = cancelPath, promise]
        (const std::shared_ptr<SharedState>& state) mutable {
        RuntimeSceneStageResult result;
        result.error = executor_shutdown_error(manifestPath);
        record_executor_result(state, result.error);
        promise->set_value(std::move(result));
    };
    task->expire = [manifestPath = cancelPath, promise]
        (const std::shared_ptr<SharedState>& state) mutable {
        RuntimeSceneStageResult result;
        result.error = executor_deadline_error(manifestPath);
        record_executor_result(state, result.error);
        promise->set_value(std::move(result));
    };

    bool rejectedStopping = false;
    bool rejectedFull = false;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping) rejectedStopping = true;
        else if (state_->sceneQueue.size() + state_->deferredQueue.size() >=
                 state_->maximumPendingJobs) {
            rejectedFull = true;
            ++state_->queueRejectedJobs;
        } else {
            task->sequence = state_->nextSequence++;
            ++state_->submittedSceneJobs;
            ++state_->submittedByPriority[priority_index(task->priority)];
            state_->sceneQueue.push_back(std::move(task));
        }
    }
    if (rejectedStopping) task->cancel(state_);
    else if (rejectedFull) {
        RuntimeSceneStageResult result;
        result.error = executor_queue_full_error(cancelPath);
        record_executor_result(state_, result.error);
        promise->set_value(std::move(result));
    } else state_->condition.notify_one();
    return future;
}

std::future<RuntimeDeferredObjectStageResult>
RuntimeSceneStagingExecutor::submit_deferred_object(
    const RuntimeSceneWorld& world,
    RuntimeSceneHandle sceneHandle,
    std::uint64_t objectId,
    RuntimeSceneStageOptions stageOptions,
    RuntimeSceneSubmissionOptions submissionOptions) {
    auto promise = std::make_shared<std::promise<RuntimeDeferredObjectStageResult>>();
    std::future<RuntimeDeferredObjectStageResult> future = promise->get_future();
    RuntimeSceneError snapshotError;
    std::optional<RuntimeSceneWorld::DeferredStageSnapshot> snapshot =
        world.snapshot_deferred_object(sceneHandle, objectId, snapshotError);
    if (!snapshot) {
        RuntimeDeferredObjectStageResult result;
        result.error = std::move(snapshotError);
        promise->set_value(std::move(result));
        return future;
    }

    const std::filesystem::path assetPath = snapshot->resolvedAssetPath;
    auto task = std::make_unique<Task>();
    task->kind = Task::Kind::Deferred;
    task->priority = submissionOptions.priority;
    task->deadline = submissionOptions.deadline;
    task->enqueuedAt = std::chrono::steady_clock::now();
    task->cancellation = stageOptions.cancellation;
    task->run = [snapshot = std::move(*snapshot), stageOptions = std::move(stageOptions), promise]
        (JobSystem&, const std::shared_ptr<SharedState>& state) mutable {
        RuntimeDeferredObjectStageResult result = RuntimeSceneWorld::stage_deferred_snapshot(
            std::move(snapshot), stageOptions);
        if (result) {
            result.staging.memoryReservation_ =
                reserve_executor_staging_memory(state, result.staging.estimatedBytes_);
            if (!result.staging.memoryReservation_) {
                result.staging = {};
                result.error = {RuntimeSceneErrorCode::StagingMemoryExceeded,
                                "persistent staging executor retained-memory budget exceeded",
                                {}, std::nullopt};
            }
        }
        record_executor_result(state, result.error);
        promise->set_value(std::move(result));
    };
    task->cancel = [assetPath, objectId, promise]
        (const std::shared_ptr<SharedState>& state) mutable {
        RuntimeDeferredObjectStageResult result;
        result.error = executor_shutdown_error(assetPath, objectId);
        record_executor_result(state, result.error);
        promise->set_value(std::move(result));
    };
    task->expire = [assetPath, objectId, promise]
        (const std::shared_ptr<SharedState>& state) mutable {
        RuntimeDeferredObjectStageResult result;
        result.error = executor_deadline_error(assetPath, objectId);
        record_executor_result(state, result.error);
        promise->set_value(std::move(result));
    };

    bool rejectedStopping = false;
    bool rejectedFull = false;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping) rejectedStopping = true;
        else if (state_->sceneQueue.size() + state_->deferredQueue.size() >=
                 state_->maximumPendingJobs) {
            rejectedFull = true;
            ++state_->queueRejectedJobs;
        } else {
            task->sequence = state_->nextSequence++;
            ++state_->submittedDeferredJobs;
            ++state_->submittedByPriority[priority_index(task->priority)];
            state_->deferredQueue.push_back(std::move(task));
        }
    }
    if (rejectedStopping) task->cancel(state_);
    else if (rejectedFull) {
        RuntimeDeferredObjectStageResult result;
        result.error = executor_queue_full_error(assetPath, objectId);
        record_executor_result(state_, result.error);
        promise->set_value(std::move(result));
    } else state_->condition.notify_one();
    return future;
}

RuntimeSceneStagingExecutorStats RuntimeSceneStagingExecutor::stats() const noexcept {
    RuntimeSceneStagingExecutorStats result;
    result.workerThreads = jobs_.worker_count();
    std::lock_guard lock(state_->mutex);
    result.queuedSceneJobs = state_->sceneQueue.size();
    result.queuedDeferredJobs = state_->deferredQueue.size();
    for (const auto& task : state_->sceneQueue) ++result.queuedByPriority[priority_index(task->priority)];
    for (const auto& task : state_->deferredQueue) ++result.queuedByPriority[priority_index(task->priority)];
    result.runningJobs = state_->runningJobs;
    result.retainedStagingBytes = state_->retainedStagingBytes;
    result.peakRetainedStagingBytes = state_->peakRetainedStagingBytes;
    result.submittedSceneJobs = state_->submittedSceneJobs;
    result.submittedDeferredJobs = state_->submittedDeferredJobs;
    result.submittedByPriority = state_->submittedByPriority;
    result.completedJobs = state_->completedJobs;
    result.failedJobs = state_->failedJobs;
    result.cancelledJobs = state_->cancelledJobs;
    result.queueRejectedJobs = state_->queueRejectedJobs;
    result.deadlineExpiredJobs = state_->deadlineExpiredJobs;
    result.backpressureWaitCount = state_->backpressureWaitCount;
    result.totalQueueWaitMicroseconds = state_->totalQueueWaitMicroseconds;
    result.maximumQueueWaitMicroseconds = state_->maximumQueueWaitMicroseconds;
    result.totalStageMicroseconds = state_->totalStageMicroseconds;
    result.maximumStageMicroseconds = state_->maximumStageMicroseconds;
    return result;
}

void RuntimeSceneStagingExecutor::coordinator_loop() {
    for (;;) {
        std::unique_ptr<Task> task;
        std::uint64_t queueWaitUs{};
        {
            std::unique_lock lock(state_->mutex);
            state_->condition.wait(lock, [this] {
                return state_->stopping || !state_->sceneQueue.empty() ||
                       !state_->deferredQueue.empty();
            });
            if (state_->stopping && state_->sceneQueue.empty() && state_->deferredQueue.empty()) {
                return;
            }
            while (!state_->stopping &&
                   state_->retainedStagingBytes >= state_->publicationBackpressureBytes &&
                   (!state_->sceneQueue.empty() || !state_->deferredQueue.empty())) {
                ++state_->backpressureWaitCount;
                state_->condition.wait(lock, [this] {
                    return state_->stopping ||
                        state_->retainedStagingBytes < state_->publicationBackpressureBytes;
                });
            }
            if (state_->stopping && state_->sceneQueue.empty() && state_->deferredQueue.empty()) {
                return;
            }

            auto sceneBest = state_->sceneQueue.empty() ? state_->sceneQueue.end() : best_task(state_->sceneQueue);
            auto deferredBest = state_->deferredQueue.empty() ? state_->deferredQueue.end() : best_task(state_->deferredQueue);
            bool takeDeferred = false;
            if (sceneBest == state_->sceneQueue.end()) takeDeferred = true;
            else if (deferredBest != state_->deferredQueue.end()) {
                if (task_precedes(**deferredBest, **sceneBest)) takeDeferred = true;
                else if (!task_precedes(**sceneBest, **deferredBest)) takeDeferred = state_->preferDeferred;
            }
            if (takeDeferred) {
                task = std::move(*deferredBest);
                state_->deferredQueue.erase(deferredBest);
                state_->preferDeferred = false;
            } else {
                task = std::move(*sceneBest);
                state_->sceneQueue.erase(sceneBest);
                state_->preferDeferred = true;
            }

            const auto now = std::chrono::steady_clock::now();
            if (task->deadline && now >= *task->deadline) {
                lock.unlock();
                task->expire(state_);
                continue;
            }
            queueWaitUs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(now - task->enqueuedAt).count());
            ++state_->runningJobs;
            state_->runningCancellation = task->cancellation;
        }

        const auto stageStart = std::chrono::steady_clock::now();
        task->run(jobs_, state_);
        const auto stageUs = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - stageStart).count());

        {
            std::lock_guard lock(state_->mutex);
            --state_->runningJobs;
            state_->runningCancellation.reset();
            state_->totalQueueWaitMicroseconds += queueWaitUs;
            state_->maximumQueueWaitMicroseconds =
                std::max(state_->maximumQueueWaitMicroseconds, queueWaitUs);
            state_->totalStageMicroseconds += stageUs;
            state_->maximumStageMicroseconds =
                std::max(state_->maximumStageMicroseconds, stageUs);
        }
        state_->condition.notify_all();
    }
}

RuntimeScenePublicationQueue::RuntimeScenePublicationQueue(
    const RuntimeScenePublicationQueueConfig& config)
    : config_(config), authorityThread_(std::this_thread::get_id()) {
    if (config_.maximumPendingPublications == 0U || config_.maximumRetainedBytes == 0U) {
        throw std::invalid_argument("publication queue limits must be positive");
    }
}

bool RuntimeScenePublicationQueue::enqueue_scene(
    RuntimeSceneStageResult&& result,
    RuntimeSceneSubmissionOptions options,
    RuntimeSceneError* error) {
    if (std::this_thread::get_id() != authorityThread_) {
        if (error) *error = {RuntimeSceneErrorCode::WrongThread,
                            "publication queue may only be modified on its authority thread", {}, {}};
        return false;
    }
    if (!result) {
        if (error) *error = result.error ? std::move(result.error) : RuntimeSceneError{
            RuntimeSceneErrorCode::InvalidStaging, "invalid scene staging result", {}, {}};
        return false;
    }
    const std::size_t bytes = result.staging.estimated_bytes();
    if (entries_.size() >= config_.maximumPendingPublications ||
        bytes > config_.maximumRetainedBytes - std::min(stats_.retainedBytes, config_.maximumRetainedBytes)) {
        ++stats_.rejected;
        if (error) *error = {RuntimeSceneErrorCode::PublicationQueueFull,
                            "authority publication queue limit reached", {}, {}};
        return false;
    }
    Entry entry;
    entry.kind = RuntimeScenePublicationKind::Scene;
    entry.options = options;
    entry.enqueuedAt = std::chrono::steady_clock::now();
    entry.sequence = nextSequence_++;
    entry.bytes = bytes;
    entry.staging = std::move(result.staging);
    entries_.push_back(std::move(entry));
    ++stats_.enqueued;
    stats_.pending = entries_.size();
    stats_.retainedBytes += bytes;
    stats_.peakRetainedBytes = std::max(stats_.peakRetainedBytes, stats_.retainedBytes);
    return true;
}

bool RuntimeScenePublicationQueue::enqueue_deferred_object(
    RuntimeDeferredObjectStageResult&& result,
    RuntimeSceneSubmissionOptions options,
    RuntimeSceneError* error) {
    if (std::this_thread::get_id() != authorityThread_) {
        if (error) *error = {RuntimeSceneErrorCode::WrongThread,
                            "publication queue may only be modified on its authority thread", {}, {}};
        return false;
    }
    if (!result) {
        if (error) *error = result.error ? std::move(result.error) : RuntimeSceneError{
            RuntimeSceneErrorCode::InvalidStaging, "invalid deferred staging result", {}, {}};
        return false;
    }
    const std::size_t bytes = result.staging.estimated_bytes();
    if (entries_.size() >= config_.maximumPendingPublications ||
        bytes > config_.maximumRetainedBytes - std::min(stats_.retainedBytes, config_.maximumRetainedBytes)) {
        ++stats_.rejected;
        if (error) *error = {RuntimeSceneErrorCode::PublicationQueueFull,
                            "authority publication queue limit reached", {}, result.staging.object_id()};
        return false;
    }
    Entry entry;
    entry.kind = RuntimeScenePublicationKind::DeferredObject;
    entry.options = options;
    entry.enqueuedAt = std::chrono::steady_clock::now();
    entry.sequence = nextSequence_++;
    entry.bytes = bytes;
    entry.staging = std::move(result.staging);
    entries_.push_back(std::move(entry));
    ++stats_.enqueued;
    stats_.pending = entries_.size();
    stats_.retainedBytes += bytes;
    stats_.peakRetainedBytes = std::max(stats_.peakRetainedBytes, stats_.retainedBytes);
    return true;
}

std::optional<RuntimeScenePublicationResult> RuntimeScenePublicationQueue::publish_next(
    RuntimeSceneWorld& world) {
    if (entries_.empty()) return std::nullopt;
    if (std::this_thread::get_id() != authorityThread_) {
        RuntimeScenePublicationResult result;
        result.error = {RuntimeSceneErrorCode::WrongThread,
                        "publication queue must be drained on its authority thread", {}, {}};
        return result;
    }
    const auto best = std::min_element(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        if (a.options.priority != b.options.priority) {
            return priority_index(a.options.priority) < priority_index(b.options.priority);
        }
        if (a.options.deadline && b.options.deadline && *a.options.deadline != *b.options.deadline) {
            return *a.options.deadline < *b.options.deadline;
        }
        if (a.options.deadline.has_value() != b.options.deadline.has_value()) {
            return a.options.deadline.has_value();
        }
        return a.sequence < b.sequence;
    });
    Entry entry = std::move(*best);
    entries_.erase(best);
    stats_.pending = entries_.size();
    stats_.retainedBytes = entry.bytes > stats_.retainedBytes ? 0U : stats_.retainedBytes - entry.bytes;

    RuntimeScenePublicationResult result;
    result.kind = entry.kind;
    const auto now = std::chrono::steady_clock::now();
    if (entry.options.deadline && now >= *entry.options.deadline) {
        ++stats_.expired;
        ++stats_.failed;
        result.error = {RuntimeSceneErrorCode::DeadlineExceeded,
                        "publication deadline expired before authority-thread commit", {}, {}};
        return result;
    }

    if (entry.kind == RuntimeScenePublicationKind::Scene) {
        RuntimeSceneLoadResult published = world.publish_staged_scene(
            std::move(std::get<RuntimeSceneStaging>(entry.staging)));
        result.sceneHandle = published.handle;
        result.error = std::move(published.error);
    } else {
        RuntimeDeferredObjectStaging staging =
            std::move(std::get<RuntimeDeferredObjectStaging>(entry.staging));
        result.sceneHandle = staging.scene_handle();
        result.objectId = staging.object_id();
        RuntimeSceneError error;
        if (!world.publish_staged_deferred_object(std::move(staging), &error)) {
            result.error = std::move(error);
        }
    }
    if (result.error) ++stats_.failed;
    else ++stats_.published;
    return result;
}

RuntimeScenePublicationQueueStats RuntimeScenePublicationQueue::stats() const noexcept {
    return stats_;
}

void RuntimeScenePublicationQueue::clear() noexcept {
    entries_.clear();
    stats_.pending = 0U;
    stats_.retainedBytes = 0U;
}

const char* to_string(RuntimeSceneErrorCode code) noexcept {
    switch (code) {
    case RuntimeSceneErrorCode::NoError: return "None";
    case RuntimeSceneErrorCode::Io: return "Io";
    case RuntimeSceneErrorCode::Json: return "Json";
    case RuntimeSceneErrorCode::InvalidManifest: return "InvalidManifest";
    case RuntimeSceneErrorCode::UnsupportedVersion: return "UnsupportedVersion";
    case RuntimeSceneErrorCode::LimitExceeded: return "LimitExceeded";
    case RuntimeSceneErrorCode::DuplicateObjectId: return "DuplicateObjectId";
    case RuntimeSceneErrorCode::DuplicateObjectIndex: return "DuplicateObjectIndex";
    case RuntimeSceneErrorCode::InvalidParent: return "InvalidParent";
    case RuntimeSceneErrorCode::CyclicHierarchy: return "CyclicHierarchy";
    case RuntimeSceneErrorCode::InvalidTransform: return "InvalidTransform";
    case RuntimeSceneErrorCode::PathEscape: return "PathEscape";
    case RuntimeSceneErrorCode::MissingAsset: return "MissingAsset";
    case RuntimeSceneErrorCode::DuplicateAssetPath: return "DuplicateAssetPath";
    case RuntimeSceneErrorCode::DvoxReadFailed: return "DvoxReadFailed";
    case RuntimeSceneErrorCode::ObjectIdMismatch: return "ObjectIdMismatch";
    case RuntimeSceneErrorCode::InvalidMaterialTable: return "InvalidMaterialTable";
    case RuntimeSceneErrorCode::InvalidMaterialReference: return "InvalidMaterialReference";
    case RuntimeSceneErrorCode::VoxelSizeMismatch: return "VoxelSizeMismatch";
    case RuntimeSceneErrorCode::DerivedDataFailure: return "DerivedDataFailure";
    case RuntimeSceneErrorCode::RendererPublicationFailure: return "RendererPublicationFailure";
    case RuntimeSceneErrorCode::PhysicsPublicationFailure: return "PhysicsPublicationFailure";
    case RuntimeSceneErrorCode::Cancelled: return "Cancelled";
    case RuntimeSceneErrorCode::StagingMemoryExceeded: return "StagingMemoryExceeded";
    case RuntimeSceneErrorCode::WrongThread: return "WrongThread";
    case RuntimeSceneErrorCode::InvalidStaging: return "InvalidStaging";
    case RuntimeSceneErrorCode::SavedStateIo: return "SavedStateIo";
    case RuntimeSceneErrorCode::SavedStateInvalid: return "SavedStateInvalid";
    case RuntimeSceneErrorCode::AssetIncompatible: return "AssetIncompatible";
    case RuntimeSceneErrorCode::StateRestoreFailure: return "StateRestoreFailure";
    case RuntimeSceneErrorCode::HotReloadIncompatible: return "HotReloadIncompatible";
    case RuntimeSceneErrorCode::HotReloadRollbackFailure: return "HotReloadRollbackFailure";
    case RuntimeSceneErrorCode::ObjectIdAlreadyReserved: return "ObjectIdAlreadyReserved";
    case RuntimeSceneErrorCode::UnknownRequestedObject: return "UnknownRequestedObject";
    case RuntimeSceneErrorCode::InvalidHandle: return "InvalidHandle";
    case RuntimeSceneErrorCode::QueueFull: return "QueueFull";
    case RuntimeSceneErrorCode::DeadlineExceeded: return "DeadlineExceeded";
    case RuntimeSceneErrorCode::PublicationQueueFull: return "PublicationQueueFull";
    }
    return "Unknown";
}

const char* to_string(RuntimeSceneJobPriority priority) noexcept {
    switch (priority) {
    case RuntimeSceneJobPriority::Critical: return "Critical";
    case RuntimeSceneJobPriority::High: return "High";
    case RuntimeSceneJobPriority::Normal: return "Normal";
    case RuntimeSceneJobPriority::Background: return "Background";
    }
    return "Unknown";
}

} // namespace dve
