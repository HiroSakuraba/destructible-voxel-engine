#include "dve/game_manifest.hpp"

#include <charconv>
#include <cmath>
#include <locale>
#include <set>
#include <sstream>
#include <system_error>

#include "dve/content_source.hpp"

namespace dve {
namespace {

[[nodiscard]] bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

[[nodiscard]] bool printable(std::string_view text) noexcept {
    for (const char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20U || byte == 0x7FU) return false;
    }
    return true;
}

[[nodiscard]] bool identifier_char(char c, bool allowPlus) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '.' || c == '-' || (allowPlus && c == '+');
}

[[nodiscard]] bool identifier(std::string_view text, bool allowPlus) noexcept {
    if (text.empty()) return false;
    for (const char c : text) if (!identifier_char(c, allowPlus)) return false;
    return true;
}

[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

[[nodiscard]] bool parse_float(std::string_view text, float& value) {
    text = trim(text);
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() && std::isfinite(value);
}

[[nodiscard]] bool parse_float3(std::string_view text, Float3& value) {
    float components[3]{};
    for (int i = 0; i < 3; ++i) {
        const std::size_t comma = text.find(',');
        if ((i < 2) != (comma != std::string_view::npos)) return false;
        const std::string_view part = i < 2 ? text.substr(0, comma) : text;
        if (!parse_float(part, components[i])) return false;
        if (i < 2) text.remove_prefix(comma + 1U);
    }
    value = {components[0], components[1], components[2]};
    return true;
}

[[nodiscard]] bool parse_camera(std::string_view text, GameManifestCamera& camera) {
    const std::size_t arrow = text.find("->");
    if (arrow == std::string_view::npos) return false;
    return parse_float3(text.substr(0, arrow), camera.eye) && parse_float3(text.substr(arrow + 2U), camera.target);
}

[[nodiscard]] bool validate_path(std::string_view key, const std::string& value, std::string* error) {
    std::string pathError;
    const auto normalized = normalize_content_path(value, &pathError);
    if (!normalized) return fail(error, "game manifest '" + std::string(key) + "' is not a valid content path: " + pathError);
    if (*normalized != value) return fail(error, "game manifest '" + std::string(key) + "' must be a normalized content path");
    return true;
}

[[nodiscard]] std::string format_float(float value) {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream.precision(9);
    stream << value;
    return stream.str();
}

} // namespace

bool GameManifest::validate(std::string* error) const {
    if (name.empty() || name.size() > kMaximumGameManifestNameBytes || !printable(name) || trim(name) != name) {
        return fail(error, "game manifest 'name' must be 1-128 printable bytes without surrounding whitespace");
    }
    if (version.size() > kMaximumGameManifestVersionBytes || !identifier(version, true)) {
        return fail(error, "game manifest 'version' must be 1-64 characters of [A-Za-z0-9._+-]");
    }
    if (!validate_path("entryScene", entryScene, error)) return false;
    if (!ends_with(entryScene, ".dvoxscene.json")) {
        return fail(error, "game manifest 'entryScene' must name a .dvoxscene.json (DVOXSCENE) file");
    }
    if (startupScript) {
        if (!validate_path("startupScript", *startupScript, error)) return false;
        if (!ends_with(*startupScript, ".lua")) return fail(error, "game manifest 'startupScript' must name a .lua file");
    }
    if (settings && !validate_path("settings", *settings, error)) return false;
    if (camera) {
        const Float3 d{camera->target.x - camera->eye.x, camera->target.y - camera->eye.y, camera->target.z - camera->eye.z};
        if (!std::isfinite(camera->eye.x) || !std::isfinite(camera->eye.y) || !std::isfinite(camera->eye.z) ||
            !std::isfinite(camera->target.x) || !std::isfinite(camera->target.y) || !std::isfinite(camera->target.z)) {
            return fail(error, "game manifest 'camera' must be finite");
        }
        if (d.x * d.x + d.y * d.y + d.z * d.z < 1.0e-12F) return fail(error, "game manifest 'camera' eye and target must differ");
    }
    if (inputBindings.size() > kMaximumGameManifestBindings) return fail(error, "game manifest has too many bind.* entries");
    for (const auto& [action, spec] : inputBindings) {
        if (action.size() > 128U || !identifier(action, false)) {
            return fail(error, "game manifest binding action '" + action + "' must be 1-128 characters of [A-Za-z0-9_.-]");
        }
        if (spec.empty() || spec.size() > kMaximumGameManifestBindingBytes || !printable(spec) || trim(spec) != spec) {
            return fail(error, "game manifest binding for '" + action + "' must be 1-1024 printable bytes");
        }
    }
    return true;
}

std::string GameManifest::serialize() const {
    std::string text = "DVE_GAME 1\n";
    text += "name=" + name + "\n";
    text += "version=" + version + "\n";
    text += "entryScene=" + entryScene + "\n";
    if (startupScript) text += "startupScript=" + *startupScript + "\n";
    if (settings) text += "settings=" + *settings + "\n";
    if (camera) {
        text += "camera=" + format_float(camera->eye.x) + "," + format_float(camera->eye.y) + "," +
                format_float(camera->eye.z) + " -> " + format_float(camera->target.x) + "," +
                format_float(camera->target.y) + "," + format_float(camera->target.z) + "\n";
    }
    for (const auto& [action, spec] : inputBindings) text += "bind." + action + "=" + spec + "\n";
    return text;
}

std::optional<GameManifest> GameManifest::parse(std::string_view text, std::string* error) {
    const auto reject = [&](std::string message) -> std::optional<GameManifest> {
        if (error) *error = std::move(message);
        return std::nullopt;
    };
    if (text.size() > kMaximumGameManifestBytes) return reject("game manifest exceeds the 64 KiB size limit");
    if (text.size() >= 3U && static_cast<unsigned char>(text[0]) == 0xEFU &&
        static_cast<unsigned char>(text[1]) == 0xBBU && static_cast<unsigned char>(text[2]) == 0xBFU) {
        text.remove_prefix(3U); // tolerate a UTF-8 BOM from Windows editors
    }
    GameManifest manifest;
    std::set<std::string, std::less<>> seen;
    bool sawHeader = false;
    std::size_t lineNumber = 0U;
    std::size_t start = 0U;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        start = end + 1U;
        ++lineNumber;
        const std::string where = "game manifest line " + std::to_string(lineNumber);
        if (line.size() > kMaximumGameManifestLineBytes) return reject(where + " exceeds the line length limit");
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        for (const char c : line) {
            const auto byte = static_cast<unsigned char>(c);
            if ((byte < 0x20U && c != '\t') || byte == 0x7FU) return reject(where + " contains a control character");
        }
        // Inline comment: '#' at line start or preceded by whitespace.
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '#' && (i == 0U || line[i - 1U] == ' ' || line[i - 1U] == '\t')) { line = line.substr(0, i); break; }
        }
        line = trim(line);
        if (line.empty()) continue;
        if (!sawHeader) {
            if (line.rfind("DVE_GAME", 0) != 0) return reject("game manifest must start with 'DVE_GAME 1'");
            const std::string_view rest = trim(line.substr(8));
            std::uint32_t version = 0U;
            const auto parsed = std::from_chars(rest.data(), rest.data() + rest.size(), version);
            if (line.size() == 8U || (line[8] != ' ' && line[8] != '\t') || parsed.ec != std::errc{} ||
                parsed.ptr != rest.data() + rest.size()) {
                return reject("game manifest header must be 'DVE_GAME <version>'");
            }
            if (version != kGameManifestVersion) return reject("unsupported game manifest version " + std::string(rest) + " (expected 1)");
            sawHeader = true;
            continue;
        }
        const std::size_t equals = line.find('=');
        if (equals == std::string_view::npos) return reject(where + " is not key=value");
        const std::string_view key = trim(line.substr(0, equals));
        const std::string value(trim(line.substr(equals + 1U)));
        if (key.empty()) return reject(where + " has an empty key");
        if (!seen.emplace(key).second) return reject(where + ": duplicate key '" + std::string(key) + "'");
        if (value.empty()) return reject(where + ": key '" + std::string(key) + "' has an empty value");
        if (key == "name") manifest.name = value;
        else if (key == "version") manifest.version = value;
        else if (key == "entryScene") manifest.entryScene = value;
        else if (key == "startupScript") manifest.startupScript = value;
        else if (key == "settings") manifest.settings = value;
        else if (key == "camera") {
            GameManifestCamera camera;
            if (!parse_camera(value, camera)) return reject(where + ": camera must be 'x,y,z -> x,y,z'");
            manifest.camera = camera;
        } else if (key.rfind("bind.", 0) == 0) {
            if (manifest.inputBindings.size() >= kMaximumGameManifestBindings) return reject("game manifest has too many bind.* entries");
            manifest.inputBindings.emplace(std::string(key.substr(5)), value);
        } else {
            return reject(where + ": unknown key '" + std::string(key) + "'");
        }
    }
    if (!sawHeader) return reject("game manifest must start with 'DVE_GAME 1'");
    if (manifest.name.empty()) return reject("game manifest is missing required key 'name'");
    if (manifest.version.empty()) return reject("game manifest is missing required key 'version'");
    if (manifest.entryScene.empty()) return reject("game manifest is missing required key 'entryScene'");
    std::string validation;
    if (!manifest.validate(&validation)) return reject(validation);
    return manifest;
}

std::string GameManifest::startup_script_or_default() const {
    return startupScript ? *startupScript : std::string(kDefaultStartupScript);
}

std::optional<GameManifest> load_game_manifest(const ContentSource& content, std::string* error) {
    ContentError readError;
    const auto text = content.read_text(kGameManifestFileName, &readError, kMaximumGameManifestBytes);
    if (!text) {
        if (error) {
            *error = std::string(kGameManifestFileName) + " in " + content.describe() + ": " +
                     (readError.message.empty() ? to_string(readError.code) : readError.message);
        }
        return std::nullopt;
    }
    std::string parseError;
    auto manifest = GameManifest::parse(*text, &parseError);
    if (!manifest && error) *error = std::string(kGameManifestFileName) + " in " + content.describe() + ": " + parseError;
    return manifest;
}

bool check_game_manifest_content(const GameManifest& manifest, const ContentSource& content, std::string* error) {
    if (!content.exists(manifest.entryScene)) {
        return fail(error, "entry scene '" + manifest.entryScene + "' is missing from " + content.describe());
    }
    if (manifest.startupScript && !content.exists(*manifest.startupScript)) {
        return fail(error, "startup script '" + *manifest.startupScript + "' is missing from " + content.describe());
    }
    if (manifest.settings && !content.exists(*manifest.settings)) {
        return fail(error, "settings file '" + *manifest.settings + "' is missing from " + content.describe());
    }
    return true;
}

} // namespace dve
