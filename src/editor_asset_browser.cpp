#include "dve/editor_asset_browser.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

namespace dve::editor {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::uint64_t kMaximumTextDependencyBytes = 16ULL * 1024ULL * 1024ULL;

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string trim(std::string value) {
    auto whitespace = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), whitespace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), whitespace).base(), value.end());
    return value;
}

std::string hex64(std::uint64_t value) {
    std::array<char, 17> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + 16, value, 16);
    return result.ec == std::errc{} ? std::string(buffer.data(), result.ptr) : std::string("0");
}

std::uint64_t fnv_append(std::uint64_t hash, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= kFnvPrime;
    }
    return hash;
}

std::uint64_t fnv_string(std::string_view text) noexcept {
    return fnv_append(kFnvOffset, text.data(), text.size());
}

bool hash_file(const std::filesystem::path& path, std::uint64_t& hash, std::uint64_t& bytes,
               std::string* error = nullptr) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error) *error = "could not open asset: " + path.string();
        return false;
    }
    hash = kFnvOffset;
    bytes = 0U;
    std::array<char, 64U * 1024U> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            hash = fnv_append(hash, buffer.data(), static_cast<std::size_t>(count));
            bytes += static_cast<std::uint64_t>(count);
        }
    }
    if (!input.eof()) {
        if (error) *error = "could not read asset: " + path.string();
        return false;
    }
    return true;
}

std::int64_t modified_ticks(const std::filesystem::path& path) noexcept {
    std::error_code error;
    const auto time = std::filesystem::last_write_time(path, error);
    return error ? 0 : static_cast<std::int64_t>(time.time_since_epoch().count());
}

bool path_starts_with(const std::filesystem::path& path, std::string_view component) {
    const auto begin = path.begin();
    return begin != path.end() && lowercase(begin->string()) == lowercase(std::string(component));
}

bool is_generated_path(const std::filesystem::path& relative) {
    return path_starts_with(relative, "cooked") || path_starts_with(relative, "artifacts") ||
           path_starts_with(relative, ".dve") || path_starts_with(relative, "build");
}

bool should_skip_directory(const std::filesystem::path& relative) {
    if (relative.empty()) return false;
    const std::string name = lowercase(relative.filename().string());
    return name == ".git" || name == ".svn" || name == ".hg" || name == ".cache" ||
           name == "third_party" || name == "node_modules" || name == "__pycache__" ||
           name.starts_with("cmake-build") || name.starts_with("build-") || name == "build";
}

bool should_skip_file(const std::filesystem::path& relative) {
    const std::string name = lowercase(relative.filename().string());
    if (name == "asset_index.dve" || name.ends_with(".tmp") || name.ends_with("~")) return true;
    if (path_starts_with(relative, ".dve") && relative.parent_path().filename() == "thumbnails") return true;
    return false;
}

std::vector<std::filesystem::path> scan_roots(const std::filesystem::path& root) {
    static constexpr std::array<std::string_view, 9> names{
        "assets", "sources", "scenes", "imports", "materials", "cooked", "scripts", "fonts", "docs"};
    std::vector<std::filesystem::path> result;
    std::error_code error;
    for (std::string_view name : names) {
        const auto candidate = root / name;
        if (std::filesystem::is_directory(candidate, error)) result.push_back(candidate);
        error.clear();
    }
    if (result.empty()) result.push_back(root);
    return result;
}

std::string make_id(const std::filesystem::path& relative, std::uint64_t contentHash,
                    std::uint64_t salt) {
    std::uint64_t hash = fnv_string(relative.generic_string());
    hash = fnv_append(hash, &contentHash, sizeof(contentHash));
    hash = fnv_append(hash, &salt, sizeof(salt));
    return "asset-" + hex64(hash);
}

std::string normalize_token(std::string token) {
    std::replace(token.begin(), token.end(), '\\', '/');
    while (token.starts_with("./")) token.erase(0U, 2U);
    while (!token.empty() && (token.front() == '/' || token.front() == '"' || token.front() == '\''))
        token.erase(token.begin());
    while (!token.empty() && (token.back() == '"' || token.back() == '\'' || token.back() == ';'))
        token.pop_back();
    return token;
}

std::vector<std::string> path_tokens(std::string_view text) {
    std::vector<std::string> result;
    std::string token;
    auto flush = [&]() {
        if (token.find('.') != std::string::npos || token.find('/') != std::string::npos ||
            token.find('\\') != std::string::npos) {
            std::string normalized = normalize_token(token);
            if (!normalized.empty()) result.push_back(std::move(normalized));
        }
        token.clear();
    };
    for (char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.' || c == '/' || c == '\\' || c == ':')
            token.push_back(static_cast<char>(c));
        else
            flush();
    }
    flush();
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::optional<std::string> read_small_text(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > kMaximumTextDependencyBytes) return std::nullopt;
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string replace_all(std::string text, std::string_view from, std::string_view to,
                        std::size_t* replacements = nullptr) {
    if (from.empty()) return text;
    std::size_t cursor = 0U;
    std::size_t count = 0U;
    while ((cursor = text.find(from, cursor)) != std::string::npos) {
        text.replace(cursor, from.size(), to);
        cursor += to.size();
        ++count;
    }
    if (replacements) *replacements = count;
    return text;
}

bool write_text_atomic(const std::filesystem::path& path, std::string_view text,
                       std::string* error = nullptr) {
    try {
        const auto temporary = path.string() + ".dve_asset_tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("could not create temporary reference file");
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) throw std::runtime_error("could not write temporary reference file");
        output.close();
        std::error_code removeError;
        std::filesystem::remove(path, removeError);
        std::filesystem::rename(temporary, path);
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}

bool contains_case_insensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    return lowercase(std::string(haystack)).find(lowercase(std::string(needle))) != std::string::npos;
}

std::array<unsigned char, 3> kind_color(EditorAssetKind kind) noexcept {
    switch (kind) {
        case EditorAssetKind::Scene: return {72U, 150U, 110U};
        case EditorAssetKind::Prefab: return {105U, 128U, 220U};
        case EditorAssetKind::PolygonMesh: return {94U, 145U, 205U};
        case EditorAssetKind::Voxel: return {180U, 115U, 65U};
        case EditorAssetKind::Material: return {160U, 105U, 185U};
        case EditorAssetKind::Texture: return {198U, 142U, 72U};
        case EditorAssetKind::Sprite: return {220U, 110U, 95U};
        case EditorAssetKind::Audio: return {75U, 170U, 178U};
        case EditorAssetKind::Script: return {192U, 184U, 80U};
        case EditorAssetKind::Font: return {190U, 105U, 110U};
        case EditorAssetKind::GaborVolume: return {100U, 125U, 205U};
        case EditorAssetKind::Text3D: return {200U, 120U, 150U};
        case EditorAssetKind::Environment: return {83U, 155U, 190U};
        case EditorAssetKind::Camera: return {120U, 170U, 110U};
        case EditorAssetKind::Animation: return {210U, 115U, 125U};
        case EditorAssetKind::Deformable: return {112U, 190U, 112U};
        case EditorAssetKind::Ui: return {70U, 178U, 154U};
        case EditorAssetKind::Project: return {145U, 145U, 155U};
        case EditorAssetKind::Document: return {170U, 160U, 130U};
        case EditorAssetKind::Terrain: return {118U, 154U, 78U};
        case EditorAssetKind::Navigation: return {72U, 176U, 205U};
        case EditorAssetKind::Other: return {115U, 120U, 130U};
    }
    return {115U, 120U, 130U};
}

std::filesystem::path normalize_relative_lexical(std::filesystem::path path) {
    path = path.lexically_normal();
    if (path == ".") path.clear();
    return path;
}

bool has_parent_escape(const std::filesystem::path& path) {
    for (const auto& component : path) if (component == "..") return true;
    return false;
}

bool tag_less(const std::string& a, const std::string& b) {
    return lowercase(a) < lowercase(b);
}

void normalize_tags(std::vector<std::string>& tags) {
    for (std::string& tag : tags) tag = lowercase(trim(std::move(tag)));
    tags.erase(std::remove_if(tags.begin(), tags.end(), [](const std::string& tag) { return tag.empty(); }), tags.end());
    std::sort(tags.begin(), tags.end(), tag_less);
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
}

} // namespace

std::string_view to_string(EditorAssetKind kind) noexcept {
    switch (kind) {
        case EditorAssetKind::Scene: return "Scene";
        case EditorAssetKind::Prefab: return "Prefab";
        case EditorAssetKind::PolygonMesh: return "Polygon Mesh";
        case EditorAssetKind::Voxel: return "Voxel";
        case EditorAssetKind::Material: return "Material";
        case EditorAssetKind::Texture: return "Texture";
        case EditorAssetKind::Sprite: return "Sprite";
        case EditorAssetKind::Audio: return "Audio";
        case EditorAssetKind::Script: return "Script";
        case EditorAssetKind::Font: return "Font";
        case EditorAssetKind::GaborVolume: return "Gabor Volume";
        case EditorAssetKind::Text3D: return "3D Text";
        case EditorAssetKind::Environment: return "Environment";
        case EditorAssetKind::Camera: return "Camera";
        case EditorAssetKind::Animation: return "Animation";
        case EditorAssetKind::Deformable: return "Deformable";
        case EditorAssetKind::Ui: return "UI";
        case EditorAssetKind::Project: return "Project";
        case EditorAssetKind::Document: return "Document";
        case EditorAssetKind::Terrain: return "Terrain";
        case EditorAssetKind::Navigation: return "Navigation";
        case EditorAssetKind::Other: return "Other";
    }
    return "Other";
}

std::string_view to_string(EditorAssetHealth health) noexcept {
    switch (health) {
        case EditorAssetHealth::Current: return "Current";
        case EditorAssetHealth::SourceOnly: return "Source Only";
        case EditorAssetHealth::Generated: return "Generated";
        case EditorAssetHealth::StaleImport: return "Stale Import";
        case EditorAssetHealth::MissingSource: return "Missing Source";
        case EditorAssetHealth::MissingCooked: return "Missing Cooked";
        case EditorAssetHealth::MissingDependency: return "Missing Dependency";
        case EditorAssetHealth::Unreadable: return "Unreadable";
    }
    return "Unreadable";
}

std::optional<EditorAssetKind> editor_asset_kind_from_string(std::string_view text) noexcept {
    const std::string value = lowercase(std::string(text));
    for (int raw = static_cast<int>(EditorAssetKind::Scene); raw <= static_cast<int>(EditorAssetKind::Other); ++raw) {
        const auto kind = static_cast<EditorAssetKind>(raw);
        if (lowercase(std::string(to_string(kind))) == value) return kind;
    }
    return std::nullopt;
}

EditorAssetKind classify_editor_asset(const std::filesystem::path& path) noexcept {
    const std::string extension = lowercase(path.extension().string());
    const std::string filename = lowercase(path.filename().string());
    if (extension == ".dnav") return EditorAssetKind::Navigation;
    if (extension == ".pgm" || extension == ".r16" || extension == ".raw" ||
        filename.ends_with(".height.png") || filename.ends_with(".heightmap.png")) return EditorAssetKind::Terrain;
    if (extension == ".dvescene") return EditorAssetKind::Scene;
    if (extension == ".dveprefab") return EditorAssetKind::Prefab;
    if (extension == ".dmesh" || extension == ".obj" || extension == ".gltf" || extension == ".glb" ||
        extension == ".fbx" || extension == ".usd" || extension == ".usdz") return EditorAssetKind::PolygonMesh;
    if (extension == ".dvox") return EditorAssetKind::Voxel;
    if (extension == ".dmat" || extension == ".material" || extension == ".dvematerial" ||
        extension == ".dvempc") return EditorAssetKind::Material;
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" || extension == ".bmp" ||
        extension == ".tga" || extension == ".hdr" || extension == ".exr" || extension == ".dds" ||
        extension == ".ktx" || extension == ".ktx2" || extension == ".ppm") return EditorAssetKind::Texture;
    if (extension == ".dvesprite") return EditorAssetKind::Sprite;
    if (extension == ".wav" || extension == ".ogg" || extension == ".flac" || extension == ".mp3" ||
        extension == ".daudio" || extension == ".dvesynth" || extension == ".dveaudio") return EditorAssetKind::Audio;
    if (extension == ".lua") return EditorAssetKind::Script;
    if (extension == ".ttf" || extension == ".otf") return EditorAssetKind::Font;
    if (extension == ".dgabor") return EditorAssetKind::GaborVolume;
    if (extension == ".dtext") return EditorAssetKind::Text3D;
    if (extension == ".dveibl" || extension == ".dveenv") return EditorAssetKind::Environment;
    if (extension == ".dvecam" || extension == ".dvecamseq") return EditorAssetKind::Camera;
    if (extension == ".dveskeleton" || extension == ".dveanim" || extension == ".dverig" ||
        extension == ".dverigui")
        return EditorAssetKind::Animation;
    if (extension == ".dvesoft") return EditorAssetKind::Deformable;
    if (extension == ".dveui") return EditorAssetKind::Ui;
    if (extension == ".dveproject") return EditorAssetKind::Project;
    if (extension == ".md" || extension == ".txt" || extension == ".json" || extension == ".toml" ||
        extension == ".yaml" || extension == ".yml") return EditorAssetKind::Document;
    return EditorAssetKind::Other;
}

bool editor_asset_is_text(const std::filesystem::path& path) noexcept {
    const std::string extension = lowercase(path.extension().string());
    static const std::unordered_set<std::string> extensions{
        ".lua", ".json", ".toml", ".ini", ".cfg", ".txt", ".md", ".dvescene", ".dveprefab", ".dveproject",
        ".dveskeleton", ".dveanim", ".dverig", ".dverigui", ".dveui", ".dvesoft", ".dvesprite",
        ".material", ".dmat", ".dvematerial", ".yaml", ".yml", ".xml", ".csv", ".hpp", ".h",
        ".cpp", ".cc", ".c", ".hlsl", ".glsl", ".vert", ".frag", ".cmake"};
    return extensions.contains(extension) || path.filename() == "CMakeLists.txt";
}

EditorAssetDatabase::EditorAssetDatabase(std::filesystem::path projectRoot) { set_project_root(std::move(projectRoot)); }

void EditorAssetDatabase::set_project_root(std::filesystem::path projectRoot) {
    std::error_code error;
    root_ = std::filesystem::weakly_canonical(projectRoot, error);
    if (error) root_ = std::filesystem::absolute(projectRoot, error).lexically_normal();
    if (error) root_ = projectRoot.lexically_normal();
    records_.clear();
}

std::filesystem::path EditorAssetDatabase::index_path() const { return root_ / ".dve" / "asset_index.dve"; }
std::filesystem::path EditorAssetDatabase::thumbnail_directory() const { return root_ / ".dve" / "thumbnails"; }

std::filesystem::path EditorAssetDatabase::normalize_relative(const std::filesystem::path& path,
                                                               std::string* error) const {
    auto fail = [&](std::string message) {
        if (error) *error = std::move(message);
        return std::filesystem::path{};
    };
    if (root_.empty()) return fail("asset database has no project root");
    std::filesystem::path relative = path;
    if (path.is_absolute()) {
        std::error_code relativeError;
        relative = std::filesystem::relative(path, root_, relativeError);
        if (relativeError) return fail("asset path is outside the project root");
    }
    relative = normalize_relative_lexical(relative);
    if (relative.empty() || relative.is_absolute() || has_parent_escape(relative))
        return fail("asset path must stay inside the project root");
    return relative;
}

bool EditorAssetDatabase::load(std::string* error) {
    records_.clear();
    if (root_.empty()) {
        if (error) *error = "asset database has no project root";
        return false;
    }
    const auto path = index_path();
    if (!std::filesystem::exists(path)) return true;
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("could not open asset index");
        std::string magic;
        std::uint32_t version{};
        std::size_t count{};
        input >> magic >> version >> count;
        if (magic != "DVE_ASSET_INDEX" || version != 1U) throw std::runtime_error("unsupported asset index format");
        std::string line;
        std::getline(input, line);
        records_.reserve(count);
        std::unordered_set<std::string> loadedIds;
        std::unordered_set<std::string> loadedPaths;
        for (std::size_t index = 0; index < count; ++index) {
            EditorAssetRecord record;
            std::string kindText;
            std::string relative;
            std::string source;
            std::string cooked;
            std::size_t tagCount{};
            input >> std::quoted(record.id) >> std::quoted(relative) >> std::quoted(kindText)
                  >> record.generation >> record.contentHash >> record.byteSize >> record.modifiedTicks
                  >> std::quoted(source) >> std::quoted(cooked) >> tagCount;
            if (!input) throw std::runtime_error("malformed asset index record");
            std::string pathError;
            record.relativePath = normalize_relative(std::filesystem::path(relative), &pathError);
            if (record.relativePath.empty()) throw std::runtime_error("unsafe asset index path: " + pathError);
            if (record.id.empty() || !loadedIds.insert(record.id).second)
                throw std::runtime_error("duplicate or empty asset ID in index");
            if (!loadedPaths.insert(record.relativePath.generic_string()).second)
                throw std::runtime_error("duplicate asset path in index");
            record.displayName = record.relativePath.filename().string();
            record.kind = editor_asset_kind_from_string(kindText).value_or(EditorAssetKind::Other);
            if (!source.empty()) {
                const auto normalizedSource = normalize_relative(std::filesystem::path(source), &pathError);
                if (normalizedSource.empty()) throw std::runtime_error("unsafe import source path: " + pathError);
                record.sourcePath = normalizedSource;
            }
            if (!cooked.empty()) {
                const auto normalizedCooked = normalize_relative(std::filesystem::path(cooked), &pathError);
                if (normalizedCooked.empty()) throw std::runtime_error("unsafe cooked asset path: " + pathError);
                record.cookedPath = normalizedCooked;
            }
            for (std::size_t tagIndex = 0; tagIndex < tagCount; ++tagIndex) {
                std::string tag;
                input >> std::quoted(tag);
                record.tags.push_back(std::move(tag));
            }
            record.generated = is_generated_path(record.relativePath);
            record.thumbnailPath = std::filesystem::path(".dve") / "thumbnails" / (record.id + ".ppm");
            records_.push_back(std::move(record));
        }
        return true;
    } catch (const std::exception& exception) {
        records_.clear();
        if (error) *error = exception.what();
        return false;
    }
}

bool EditorAssetDatabase::save(std::string* error) const {
    try {
        if (root_.empty()) throw std::runtime_error("asset database has no project root");
        std::filesystem::create_directories(index_path().parent_path());
        const auto temporary = index_path().string() + ".tmp";
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("could not create asset index");
        output << "DVE_ASSET_INDEX 1 " << records_.size() << '\n';
        std::vector<const EditorAssetRecord*> ordered;
        ordered.reserve(records_.size());
        for (const auto& record : records_) ordered.push_back(&record);
        std::sort(ordered.begin(), ordered.end(), [](const EditorAssetRecord* a, const EditorAssetRecord* b) {
            return a->relativePath.generic_string() < b->relativePath.generic_string();
        });
        for (const EditorAssetRecord* record : ordered) {
            output << std::quoted(record->id) << ' ' << std::quoted(record->relativePath.generic_string()) << ' '
                   << std::quoted(std::string(to_string(record->kind))) << ' ' << record->generation << ' '
                   << record->contentHash << ' ' << record->byteSize << ' ' << record->modifiedTicks << ' '
                   << std::quoted(record->sourcePath ? record->sourcePath->generic_string() : std::string{}) << ' '
                   << std::quoted(record->cookedPath ? record->cookedPath->generic_string() : std::string{}) << ' '
                   << record->tags.size();
            for (const std::string& tag : record->tags) output << ' ' << std::quoted(tag);
            output << '\n';
        }
        output.flush();
        if (!output) throw std::runtime_error("could not write asset index");
        output.close();
        std::error_code removeError;
        std::filesystem::remove(index_path(), removeError);
        std::filesystem::rename(temporary, index_path());
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}

bool EditorAssetDatabase::scan(EditorAssetScanReport* report, std::string* error) {
    EditorAssetScanReport local;
    if (!report) report = &local;
    *report = {};
    if (root_.empty()) {
        if (error) *error = "asset database has no project root";
        return false;
    }
    std::vector<EditorAssetRecord> previous;
    std::string loadError;
    if (!load(&loadError)) report->warnings.push_back("Ignoring unreadable previous index: " + loadError);
    previous = records_;

    std::unordered_map<std::string, std::size_t> previousByPath;
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> previousByFingerprint;
    std::unordered_set<std::string> usedIds;
    for (std::size_t index = 0; index < previous.size(); ++index) {
        previousByPath.emplace(previous[index].relativePath.generic_string(), index);
        const std::uint64_t key = previous[index].contentHash ^ (previous[index].byteSize * 0x9e3779b97f4a7c15ULL) ^
                                  (static_cast<std::uint64_t>(previous[index].kind) << 56U);
        previousByFingerprint[key].push_back(index);
        usedIds.insert(previous[index].id);
    }
    std::vector<bool> previousUsed(previous.size(), false);
    std::vector<EditorAssetRecord> discovered;
    std::set<std::string> seenPaths;
    std::error_code iterationError;
    for (const auto& base : scan_roots(root_)) {
        std::filesystem::recursive_directory_iterator iterator(
            base, std::filesystem::directory_options::skip_permission_denied, iterationError);
        const std::filesystem::recursive_directory_iterator end;
        while (!iterationError && iterator != end) {
            const auto entry = *iterator;
            std::error_code statusError;
            std::filesystem::path relative = std::filesystem::relative(entry.path(), root_, statusError);
            if (statusError) {
                iterator.increment(iterationError);
                continue;
            }
            relative = normalize_relative_lexical(relative);
            if (entry.is_directory(statusError)) {
                if (should_skip_directory(relative)) iterator.disable_recursion_pending();
                iterator.increment(iterationError);
                continue;
            }
            if (!entry.is_regular_file(statusError) || should_skip_file(relative) ||
                !seenPaths.insert(relative.generic_string()).second) {
                iterator.increment(iterationError);
                continue;
            }
            EditorAssetRecord record;
            record.relativePath = relative;
            record.displayName = relative.filename().string();
            record.kind = classify_editor_asset(relative);
            record.generated = is_generated_path(relative);
            record.modifiedTicks = modified_ticks(entry.path());
            std::string hashError;
            if (!hash_file(entry.path(), record.contentHash, record.byteSize, &hashError)) {
                record.health = EditorAssetHealth::Unreadable;
                report->warnings.push_back(hashError);
            }
            const auto previousPath = previousByPath.find(relative.generic_string());
            if (previousPath != previousByPath.end()) {
                const auto& old = previous[previousPath->second];
                previousUsed[previousPath->second] = true;
                record.id = old.id;
                record.generation = old.contentHash == record.contentHash && old.byteSize == record.byteSize
                    ? old.generation : old.generation + 1U;
                record.tags = old.tags;
                record.sourcePath = old.sourcePath;
                record.cookedPath = old.cookedPath;
                if (record.generation != old.generation) ++report->changed;
            } else {
                const std::uint64_t fingerprint = record.contentHash ^ (record.byteSize * 0x9e3779b97f4a7c15ULL) ^
                                                  (static_cast<std::uint64_t>(record.kind) << 56U);
                std::optional<std::size_t> movedFrom;
                if (const auto candidates = previousByFingerprint.find(fingerprint); candidates != previousByFingerprint.end()) {
                    for (std::size_t candidate : candidates->second) {
                        if (!previousUsed[candidate] && previous[candidate].contentHash == record.contentHash &&
                            previous[candidate].byteSize == record.byteSize && previous[candidate].kind == record.kind) {
                            if (movedFrom) { movedFrom.reset(); break; }
                            movedFrom = candidate;
                        }
                    }
                }
                if (movedFrom) {
                    const auto& old = previous[*movedFrom];
                    previousUsed[*movedFrom] = true;
                    record.id = old.id;
                    record.generation = old.generation;
                    record.tags = old.tags;
                    record.sourcePath = old.sourcePath;
                    record.cookedPath = old.cookedPath;
                    for (auto* linked : {&record.sourcePath, &record.cookedPath})
                        if (*linked && **linked == old.relativePath) *linked = record.relativePath;
                    ++report->moved;
                } else {
                    std::uint64_t salt = 0U;
                    do record.id = make_id(relative, record.contentHash, salt++); while (usedIds.contains(record.id));
                    usedIds.insert(record.id);
                    ++report->added;
                }
            }
            record.thumbnailPath = std::filesystem::path(".dve") / "thumbnails" / (record.id + ".ppm");
            discovered.push_back(std::move(record));
            iterator.increment(iterationError);
        }
        if (iterationError) {
            report->warnings.push_back("Asset scan stopped in " + base.string() + ": " + iterationError.message());
            iterationError.clear();
        }
    }
    report->removed = static_cast<std::size_t>(std::count(previousUsed.begin(), previousUsed.end(), false));
    records_ = std::move(discovered);
    refresh_health_and_dependencies(report);
    report->indexed = records_.size();
    if (!save(error)) return false;
    report->thumbnailsGenerated = generate_missing_thumbnails(nullptr);
    return true;
}

void EditorAssetDatabase::refresh_health_and_dependencies(EditorAssetScanReport* report) {
    std::unordered_map<std::string, std::string> idByPath;
    std::unordered_map<std::string, std::vector<std::string>> idsByFilename;
    for (const auto& record : records_) {
        idByPath.emplace(record.relativePath.generic_string(), record.id);
        idsByFilename[record.relativePath.filename().generic_string()].push_back(record.id);
    }
    for (auto& record : records_) {
        record.dependencies.clear();
        record.unresolvedDependencies.clear();
        if (!editor_asset_is_text(record.relativePath)) continue;
        const auto text = read_small_text(root_ / record.relativePath);
        if (!text) continue;
        for (const std::string& token : path_tokens(*text)) {
            std::optional<std::string> dependency;
            if (const auto exact = idByPath.find(token); exact != idByPath.end()) dependency = exact->second;
            else if (const auto byName = idsByFilename.find(std::filesystem::path(token).filename().generic_string());
                     byName != idsByFilename.end() && byName->second.size() == 1U) dependency = byName->second.front();
            if (dependency && *dependency != record.id) {
                record.dependencies.push_back(*dependency);
            } else if (!dependency && classify_editor_asset(std::filesystem::path(token)) != EditorAssetKind::Other) {
                record.unresolvedDependencies.emplace_back(token);
            }
        }
        std::sort(record.dependencies.begin(), record.dependencies.end());
        record.dependencies.erase(std::unique(record.dependencies.begin(), record.dependencies.end()), record.dependencies.end());
        std::sort(record.unresolvedDependencies.begin(), record.unresolvedDependencies.end());
        record.unresolvedDependencies.erase(std::unique(record.unresolvedDependencies.begin(), record.unresolvedDependencies.end()),
                                             record.unresolvedDependencies.end());
    }
    std::unordered_set<std::string> knownIds;
    for (const auto& record : records_) knownIds.insert(record.id);
    for (auto& record : records_) {
        record.health = record.generated ? EditorAssetHealth::Generated : EditorAssetHealth::Current;
        if (record.sourcePath || record.cookedPath) {
            const bool sourceExists = record.sourcePath && std::filesystem::exists(root_ / *record.sourcePath);
            const bool cookedExists = record.cookedPath && std::filesystem::exists(root_ / *record.cookedPath);
            if (record.sourcePath && !sourceExists) record.health = EditorAssetHealth::MissingSource;
            else if (record.cookedPath && !cookedExists) record.health = EditorAssetHealth::MissingCooked;
            else if (sourceExists && cookedExists && modified_ticks(root_ / *record.sourcePath) > modified_ticks(root_ / *record.cookedPath))
                record.health = EditorAssetHealth::StaleImport;
            else record.health = EditorAssetHealth::Current;
        } else if (!record.generated) {
            const std::string extension = lowercase(record.relativePath.extension().string());
            if (extension == ".obj" || extension == ".gltf" || extension == ".glb" || extension == ".fbx" ||
                extension == ".usd" || extension == ".usdz") record.health = EditorAssetHealth::SourceOnly;
        }
        const bool broken = !record.unresolvedDependencies.empty() ||
            std::any_of(record.dependencies.begin(), record.dependencies.end(), [&](const std::string& id) {
                return !knownIds.contains(id);
            });
        if (broken) {
            record.health = EditorAssetHealth::MissingDependency;
            if (report) report->brokenDependencies += std::max<std::size_t>(1U, record.unresolvedDependencies.size());
        }
    }
}

const EditorAssetRecord* EditorAssetDatabase::find(std::string_view id) const noexcept {
    const auto found = std::find_if(records_.begin(), records_.end(), [&](const EditorAssetRecord& record) { return record.id == id; });
    return found == records_.end() ? nullptr : &*found;
}
EditorAssetRecord* EditorAssetDatabase::find(std::string_view id) noexcept {
    const auto found = std::find_if(records_.begin(), records_.end(), [&](const EditorAssetRecord& record) { return record.id == id; });
    return found == records_.end() ? nullptr : &*found;
}
const EditorAssetRecord* EditorAssetDatabase::find_path(const std::filesystem::path& relativePath) const noexcept {
    const auto normalized = normalize_relative_lexical(relativePath).generic_string();
    const auto found = std::find_if(records_.begin(), records_.end(), [&](const EditorAssetRecord& record) {
        return record.relativePath.generic_string() == normalized;
    });
    return found == records_.end() ? nullptr : &*found;
}

std::vector<const EditorAssetRecord*> EditorAssetDatabase::query(const EditorAssetQuery& requested) const {
    std::vector<const EditorAssetRecord*> result;
    for (const auto& record : records_) {
        if (!requested.includeGenerated && record.generated) continue;
        if (requested.kind && record.kind != *requested.kind) continue;
        if (requested.staleOnly && record.health != EditorAssetHealth::StaleImport &&
            record.health != EditorAssetHealth::MissingSource && record.health != EditorAssetHealth::MissingCooked &&
            record.health != EditorAssetHealth::MissingDependency && record.health != EditorAssetHealth::Unreadable) continue;
        if (!requested.text.empty() && !contains_case_insensitive(record.displayName, requested.text) &&
            !contains_case_insensitive(record.relativePath.generic_string(), requested.text) &&
            !contains_case_insensitive(to_string(record.kind), requested.text) &&
            !std::any_of(record.tags.begin(), record.tags.end(), [&](const std::string& tag) {
                return contains_case_insensitive(tag, requested.text);
            })) continue;
        bool hasTags = true;
        for (const std::string& requestedTag : requested.requiredTags) {
            const std::string tag = lowercase(requestedTag);
            if (std::none_of(record.tags.begin(), record.tags.end(), [&](const std::string& actual) {
                return lowercase(actual) == tag;
            })) { hasTags = false; break; }
        }
        if (!hasTags) continue;
        result.push_back(&record);
    }
    auto compare = [&](const EditorAssetRecord* a, const EditorAssetRecord* b) {
        int ordering = 0;
        switch (requested.sort) {
            case EditorAssetSort::Name: ordering = lowercase(a->displayName).compare(lowercase(b->displayName)); break;
            case EditorAssetSort::Path: ordering = a->relativePath.generic_string().compare(b->relativePath.generic_string()); break;
            case EditorAssetSort::Kind: ordering = std::string(to_string(a->kind)).compare(std::string(to_string(b->kind))); break;
            case EditorAssetSort::Modified: ordering = a->modifiedTicks < b->modifiedTicks ? -1 : a->modifiedTicks > b->modifiedTicks ? 1 : 0; break;
            case EditorAssetSort::Size: ordering = a->byteSize < b->byteSize ? -1 : a->byteSize > b->byteSize ? 1 : 0; break;
        }
        if (ordering == 0) ordering = a->relativePath.generic_string().compare(b->relativePath.generic_string());
        return requested.descending ? ordering > 0 : ordering < 0;
    };
    std::stable_sort(result.begin(), result.end(), compare);
    if (result.size() > requested.limit) result.resize(requested.limit);
    return result;
}

std::vector<const EditorAssetRecord*> EditorAssetDatabase::dependencies_of(std::string_view id) const {
    std::vector<const EditorAssetRecord*> result;
    if (const auto* record = find(id)) for (const std::string& dependency : record->dependencies)
        if (const auto* target = find(dependency)) result.push_back(target);
    return result;
}
std::vector<const EditorAssetRecord*> EditorAssetDatabase::reverse_dependencies_of(std::string_view id) const {
    std::vector<const EditorAssetRecord*> result;
    for (const auto& record : records_)
        if (std::find(record.dependencies.begin(), record.dependencies.end(), id) != record.dependencies.end()) result.push_back(&record);
    return result;
}

bool EditorAssetDatabase::set_tags(std::string_view id, std::vector<std::string> tags, std::string* error) {
    EditorAssetRecord* record = find(id);
    if (!record) {
        if (error) *error = "asset does not exist";
        return false;
    }
    normalize_tags(tags);
    record->tags = std::move(tags);
    return save(error);
}

bool EditorAssetDatabase::register_import(const std::filesystem::path& sourceRelative,
                                          const std::filesystem::path& cookedRelative,
                                          std::string* error) {
    const auto source = normalize_relative(sourceRelative, error);
    if (source.empty()) return false;
    const auto cooked = normalize_relative(cookedRelative, error);
    if (cooked.empty()) return false;
    EditorAssetRecord* sourceRecord = nullptr;
    EditorAssetRecord* cookedRecord = nullptr;
    for (auto& record : records_) {
        if (record.relativePath == source) sourceRecord = &record;
        if (record.relativePath == cooked) cookedRecord = &record;
    }
    if (!sourceRecord || !cookedRecord) {
        if (error) *error = "source and cooked assets must both be indexed before registering an import";
        return false;
    }
    sourceRecord->sourcePath = source;
    sourceRecord->cookedPath = cooked;
    cookedRecord->sourcePath = source;
    cookedRecord->cookedPath = cooked;
    refresh_health_and_dependencies(nullptr);
    return save(error);
}

EditorAssetMutationReport EditorAssetDatabase::move_asset(std::string_view id,
                                                           const std::filesystem::path& newRelativePath,
                                                           bool rewriteTextReferences) {
    EditorAssetMutationReport result;
    EditorAssetRecord* record = find(id);
    if (!record) { result.message = "asset does not exist"; return result; }
    std::string pathError;
    const auto targetRelative = normalize_relative(newRelativePath, &pathError);
    if (targetRelative.empty()) { result.message = pathError; return result; }
    if (path_starts_with(targetRelative, ".dve")) { result.message = "assets cannot be moved into the internal .dve directory"; return result; }
    const auto sourceAbsolute = root_ / record->relativePath;
    const auto targetAbsolute = root_ / targetRelative;
    if (std::filesystem::exists(targetAbsolute)) { result.message = "target asset already exists"; return result; }
    result.oldPath = record->relativePath;
    result.newPath = targetRelative;

    struct ReferenceRewrite { std::filesystem::path path; std::string before; std::string after; };
    std::vector<ReferenceRewrite> rewrites;
    const std::string oldPath = record->relativePath.generic_string();
    const std::string newPath = targetRelative.generic_string();
    if (rewriteTextReferences) {
        for (const EditorAssetRecord* referrer : reverse_dependencies_of(id)) {
            if (!editor_asset_is_text(referrer->relativePath)) {
                result.unresolvedReferences.push_back(referrer->relativePath);
                continue;
            }
            const auto before = read_small_text(root_ / referrer->relativePath);
            if (!before) { result.unresolvedReferences.push_back(referrer->relativePath); continue; }
            std::size_t replacements{};
            std::string after = replace_all(*before, oldPath, newPath, &replacements);
            if (replacements == 0U && record->relativePath.filename() != targetRelative.filename()) {
                const std::string oldName = record->relativePath.filename().generic_string();
                const std::string newName = targetRelative.filename().generic_string();
                const auto sameName = std::count_if(records_.begin(), records_.end(), [&](const EditorAssetRecord& candidate) {
                    return candidate.relativePath.filename() == record->relativePath.filename();
                });
                if (sameName == 1) after = replace_all(std::move(after), oldName, newName, &replacements);
            }
            if (replacements > 0U) rewrites.push_back({referrer->relativePath, *before, std::move(after)});
            else result.unresolvedReferences.push_back(referrer->relativePath);
        }
    }

    try {
        std::filesystem::create_directories(targetAbsolute.parent_path());
        std::filesystem::rename(sourceAbsolute, targetAbsolute);
        std::vector<std::filesystem::path> written;
        for (const auto& rewrite : rewrites) {
            std::string rewriteError;
            if (!write_text_atomic(root_ / rewrite.path, rewrite.after, &rewriteError)) {
                for (const auto& restore : rewrites)
                    if (std::find(written.begin(), written.end(), restore.path) != written.end())
                        (void)write_text_atomic(root_ / restore.path, restore.before, nullptr);
                std::filesystem::rename(targetAbsolute, sourceAbsolute);
                result.message = "reference rewrite failed: " + rewriteError;
                return result;
            }
            written.push_back(rewrite.path);
            result.rewrittenReferences.push_back(rewrite.path);
        }
        const std::filesystem::path oldRelative = result.oldPath;
        for (auto& candidate : records_) {
            if (candidate.relativePath == oldRelative) candidate.relativePath = targetRelative;
            if (candidate.sourcePath && *candidate.sourcePath == oldRelative) candidate.sourcePath = targetRelative;
            if (candidate.cookedPath && *candidate.cookedPath == oldRelative) candidate.cookedPath = targetRelative;
        }
        record = find(id);
        if (record) {
            record->displayName = targetRelative.filename().string();
            record->modifiedTicks = modified_ticks(targetAbsolute);
        }
        std::string saveError;
        if (!save(&saveError)) {
            for (const auto& rewrite : rewrites) (void)write_text_atomic(root_ / rewrite.path, rewrite.before, nullptr);
            std::filesystem::rename(targetAbsolute, sourceAbsolute);
            result.message = "asset index update failed: " + saveError;
            return result;
        }
        EditorAssetScanReport scanReport;
        std::string scanError;
        if (!scan(&scanReport, &scanError)) {
            result.message = "asset moved but rescan failed: " + scanError;
            return result;
        }
        result.success = true;
        result.message = "asset moved and references updated";
        return result;
    } catch (const std::exception& exception) {
        result.message = exception.what();
        return result;
    }
}

bool EditorAssetDatabase::generate_thumbnail(std::string_view id, std::string* error) {
    EditorAssetRecord* record = find(id);
    if (!record) {
        if (error) *error = "asset does not exist";
        return false;
    }
    try {
        constexpr int width = 64;
        constexpr int height = 64;
        std::filesystem::create_directories(thumbnail_directory());
        const auto outputPath = root_ / record->thumbnailPath;
        const auto base = kind_color(record->kind);
        std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("could not create asset thumbnail");
        output << "P6\n" << width << ' ' << height << "\n255\n";
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const bool border = x < 3 || y < 3 || x >= width - 3 || y >= height - 3;
                const bool checker = ((x / 8) + (y / 8)) % 2 == 0;
                const bool diagonal = ((x + y + static_cast<int>(record->contentHash & 15U)) % 19) < 3;
                std::array<unsigned char, 3> pixel{};
                for (std::size_t channel = 0; channel < 3U; ++channel) {
                    const unsigned value = border ? 28U : diagonal ? std::min(255U, static_cast<unsigned>(base[channel]) + 45U)
                        : checker ? base[channel] : static_cast<unsigned>(base[channel]) * 3U / 4U;
                    pixel[channel] = static_cast<unsigned char>(value);
                }
                output.write(reinterpret_cast<const char*>(pixel.data()), 3);
            }
        }
        if (!output) throw std::runtime_error("could not write asset thumbnail");
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}

std::size_t EditorAssetDatabase::generate_missing_thumbnails(std::string* error) {
    std::size_t count{};
    for (const auto& record : records_) {
        if (std::filesystem::exists(root_ / record.thumbnailPath)) continue;
        std::string localError;
        if (!generate_thumbnail(record.id, &localError)) {
            if (error) *error = localError;
            return count;
        }
        ++count;
    }
    return count;
}

} // namespace dve::editor
