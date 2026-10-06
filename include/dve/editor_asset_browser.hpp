#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dve::editor {

enum class EditorAssetKind : std::uint8_t {
    Scene,
    Prefab,
    PolygonMesh,
    Voxel,
    Material,
    Texture,
    Sprite,
    Audio,
    Script,
    Font,
    GaborVolume,
    Text3D,
    Environment,
    Camera,
    Animation,
    Deformable,
    Ui,
    Project,
    Document,
    Terrain,
    Navigation,
    Other,
};

enum class EditorAssetHealth : std::uint8_t {
    Current,
    SourceOnly,
    Generated,
    StaleImport,
    MissingSource,
    MissingCooked,
    MissingDependency,
    Unreadable,
};

enum class EditorAssetSort : std::uint8_t { Name, Path, Kind, Modified, Size };

struct EditorAssetRecord {
    std::string id;
    std::filesystem::path relativePath;
    std::string displayName;
    EditorAssetKind kind{EditorAssetKind::Other};
    EditorAssetHealth health{EditorAssetHealth::Current};
    std::uint64_t byteSize{};
    std::uint64_t contentHash{};
    std::uint64_t generation{1U};
    std::int64_t modifiedTicks{};
    bool generated{};
    std::vector<std::string> tags;
    std::vector<std::string> dependencies;
    std::vector<std::filesystem::path> unresolvedDependencies;
    std::optional<std::filesystem::path> sourcePath;
    std::optional<std::filesystem::path> cookedPath;
    std::filesystem::path thumbnailPath;
};

struct EditorAssetQuery {
    std::string text;
    std::optional<EditorAssetKind> kind;
    std::vector<std::string> requiredTags;
    bool includeGenerated{true};
    bool staleOnly{};
    EditorAssetSort sort{EditorAssetSort::Name};
    bool descending{};
    std::size_t limit{512U};
};

struct EditorAssetScanReport {
    std::size_t indexed{};
    std::size_t added{};
    std::size_t changed{};
    std::size_t moved{};
    std::size_t removed{};
    std::size_t brokenDependencies{};
    std::size_t thumbnailsGenerated{};
    // Files whose content was read and hashed, and files whose hash was reused
    // because their size and modification time were unchanged since the last scan.
    std::size_t hashedFiles{};
    std::size_t reusedHashes{};
    std::vector<std::string> warnings;
};

struct EditorAssetScanOptions {
    // Write missing thumbnails before scan() returns. The editor turns this off and
    // fills them in a little per frame with generate_missing_thumbnails_for().
    bool generateThumbnails{true};
};

struct EditorAssetMutationReport {
    bool success{};
    std::string message;
    std::filesystem::path oldPath;
    std::filesystem::path newPath;
    std::vector<std::filesystem::path> rewrittenReferences;
    std::vector<std::filesystem::path> unresolvedReferences;
};

struct EditorAssetBrowserState {
    EditorAssetQuery query;
    std::optional<std::string> selectedId;
    std::size_t firstVisible{};
};

class EditorAssetDatabase {
public:
    EditorAssetDatabase() = default;
    explicit EditorAssetDatabase(std::filesystem::path projectRoot);

    void set_project_root(std::filesystem::path projectRoot);
    [[nodiscard]] const std::filesystem::path& project_root() const noexcept { return root_; }
    [[nodiscard]] bool ready() const noexcept { return !root_.empty(); }

    [[nodiscard]] bool scan(EditorAssetScanReport* report = nullptr, std::string* error = nullptr);
    [[nodiscard]] bool scan(EditorAssetScanReport* report, std::string* error, const EditorAssetScanOptions& options);
    [[nodiscard]] bool save(std::string* error = nullptr) const;
    [[nodiscard]] bool load(std::string* error = nullptr);

    [[nodiscard]] const std::vector<EditorAssetRecord>& records() const noexcept { return records_; }
    [[nodiscard]] std::vector<const EditorAssetRecord*> query(const EditorAssetQuery& query) const;
    [[nodiscard]] const EditorAssetRecord* find(std::string_view id) const noexcept;
    [[nodiscard]] EditorAssetRecord* find(std::string_view id) noexcept;
    [[nodiscard]] const EditorAssetRecord* find_path(const std::filesystem::path& relativePath) const noexcept;
    [[nodiscard]] std::vector<const EditorAssetRecord*> dependencies_of(std::string_view id) const;
    [[nodiscard]] std::vector<const EditorAssetRecord*> reverse_dependencies_of(std::string_view id) const;

    [[nodiscard]] bool set_tags(std::string_view id, std::vector<std::string> tags,
                                std::string* error = nullptr);
    [[nodiscard]] bool register_import(const std::filesystem::path& sourceRelative,
                                       const std::filesystem::path& cookedRelative,
                                       std::string* error = nullptr);
    [[nodiscard]] EditorAssetMutationReport move_asset(std::string_view id,
                                                        const std::filesystem::path& newRelativePath,
                                                        bool rewriteTextReferences = true);
    [[nodiscard]] bool generate_thumbnail(std::string_view id, std::string* error = nullptr);
    [[nodiscard]] std::size_t generate_missing_thumbnails(std::string* error = nullptr);
    // Resumable: checks and writes missing thumbnails until the budget is spent and
    // returns true once every record has been visited since the last scan or load.
    [[nodiscard]] bool generate_missing_thumbnails_for(std::chrono::microseconds budget,
                                                       std::size_t* generated = nullptr,
                                                       std::string* error = nullptr);

    [[nodiscard]] std::filesystem::path index_path() const;
    [[nodiscard]] std::filesystem::path thumbnail_directory() const;

private:
    [[nodiscard]] std::filesystem::path normalize_relative(const std::filesystem::path& path,
                                                           std::string* error = nullptr) const;
    void refresh_health_and_dependencies(EditorAssetScanReport* report);
    [[nodiscard]] bool write_thumbnail(const EditorAssetRecord& record, std::string* error) const;

    // Dependency tokens of text assets by relative path, valid while the content
    // hash and size match. In memory only; rebuilt on the first scan of a session.
    struct TextTokens {
        std::uint64_t contentHash{};
        std::uint64_t byteSize{};
        std::vector<std::string> tokens;
    };
    std::unordered_map<std::string, TextTokens> textTokens_;

    std::filesystem::path root_;
    std::vector<EditorAssetRecord> records_;
    std::size_t thumbnailCursor_{};
};

[[nodiscard]] std::string_view to_string(EditorAssetKind kind) noexcept;
[[nodiscard]] std::string_view to_string(EditorAssetHealth health) noexcept;
[[nodiscard]] std::optional<EditorAssetKind> editor_asset_kind_from_string(std::string_view text) noexcept;
[[nodiscard]] EditorAssetKind classify_editor_asset(const std::filesystem::path& path) noexcept;
[[nodiscard]] bool editor_asset_is_text(const std::filesystem::path& path) noexcept;

} // namespace dve::editor
