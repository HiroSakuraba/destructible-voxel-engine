#include "dve/editor_file_workflow.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace dve::editor {
namespace {

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool ends_with_case_insensitive(std::string value, std::string suffix) {
    value = lower_ascii(std::move(value));
    suffix = lower_ascii(std::move(suffix));
    return value.ends_with(suffix);
}

std::string default_display_name(const std::filesystem::path& projectFile) {
    const std::string stem = projectFile.stem().string();
    return stem.empty() ? projectFile.filename().string() : stem;
}

} // namespace

EditorFileRoute classify_editor_file(const std::filesystem::path& path) {
    EditorFileRoute route;
    route.path = path;
    const std::string filename = path.filename().string();
    const std::string extension = lower_ascii(path.extension().string());

    if (extension == ".dveproject") {
        route.intent = EditorFileIntent::OpenProject;
        route.actionLabel = "Open project";
    } else if (extension == ".dvescene") {
        route.intent = EditorFileIntent::OpenScene;
        route.actionLabel = "Open editable scene";
    } else if (extension == ".dveui") {
        route.intent = EditorFileIntent::OpenUiAsset;
        route.actionLabel = "Open gameplay UI asset";
    } else if (extension == ".dvesprite") {
        route.intent = EditorFileIntent::OpenSpriteAsset;
        route.actionLabel = "Open sprite asset";
    } else if (extension == ".dvesoft") {
        route.intent = EditorFileIntent::OpenDeformableAsset;
        route.actionLabel = "Open deformable asset";
    } else if (extension == ".glb" || extension == ".gltf" || extension == ".obj") {
        route.intent = EditorFileIntent::ImportModel;
        route.actionLabel = "Import model and open voxel preview";
    } else if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
               extension == ".ktx2") {
        route.intent = EditorFileIntent::ImportTexture;
        route.actionLabel = "Import texture";
    } else if (extension == ".dvematerials") {
        route.intent = EditorFileIntent::OpenMaterialLibrary;
        route.actionLabel = "Open material library";
    } else if (extension == ".dveimport") {
        route.intent = EditorFileIntent::OpenImportRecipe;
        route.actionLabel = "Open import recipe";
    } else if (extension == ".dvox") {
        route.intent = EditorFileIntent::AddVoxelAsset;
        route.actionLabel = "Add cooked voxel asset";
    } else if (ends_with_case_insensitive(filename, ".dvoxscene.json")) {
        route.intent = EditorFileIntent::AddScenePackage;
        route.actionLabel = "Add cooked scene package";
    } else {
        route.intent = EditorFileIntent::Unsupported;
        route.actionLabel = "Unsupported file";
    }
    route.supported = route.intent != EditorFileIntent::Unsupported;
    return route;
}

std::vector<EditorFileRoute> route_editor_file_drop(
    const std::vector<std::filesystem::path>& paths) {
    std::vector<EditorFileRoute> result;
    result.reserve(paths.size());
    for (const std::filesystem::path& path : paths)
        result.push_back(classify_editor_file(path));
    return result;
}

RecentProjectRegistry::RecentProjectRegistry(std::size_t capacity)
    : capacity_(std::max<std::size_t>(1, capacity)) {}

std::filesystem::path RecentProjectRegistry::normalized_path(
    const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec) absolute = path;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(absolute, ec);
    return (ec ? absolute.lexically_normal() : canonical).lexically_normal();
}

void RecentProjectRegistry::record(
    std::filesystem::path projectFile, std::string displayName) {
    projectFile = normalized_path(projectFile);
    if (displayName.empty()) displayName = default_display_name(projectFile);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const RecentProjectEntry& entry) {
        return normalized_path(entry.projectFile) == projectFile;
    }), entries_.end());
    entries_.insert(entries_.begin(), {std::move(projectFile), std::move(displayName)});
    if (entries_.size() > capacity_) entries_.resize(capacity_);
}

bool RecentProjectRegistry::remove(const std::filesystem::path& projectFile) {
    const std::filesystem::path normalized = normalized_path(projectFile);
    const std::size_t before = entries_.size();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const RecentProjectEntry& entry) {
        return normalized_path(entry.projectFile) == normalized;
    }), entries_.end());
    return entries_.size() != before;
}

std::size_t RecentProjectRegistry::remove_missing() {
    const std::size_t before = entries_.size();
    std::error_code ec;
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [&](const RecentProjectEntry& entry) {
        ec.clear();
        return !std::filesystem::is_regular_file(entry.projectFile, ec) || ec;
    }), entries_.end());
    return before - entries_.size();
}

bool RecentProjectRegistry::save(const std::filesystem::path& path, std::string* error) const {
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error) *error = "could not create recent-project directory";
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        if (error) *error = "could not open recent-project registry";
        return false;
    }
    output << "DVE_RECENT_PROJECTS 1\n";
    for (const RecentProjectEntry& entry : entries_)
        output << std::quoted(entry.displayName) << ' ' << std::quoted(entry.projectFile.generic_string()) << '\n';
    if (!output) {
        if (error) *error = "could not write recent-project registry";
        return false;
    }
    return true;
}

std::optional<RecentProjectRegistry> RecentProjectRegistry::load(
    const std::filesystem::path& path,
    std::size_t capacity,
    std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (error) *error = "could not open recent-project registry";
        return std::nullopt;
    }
    std::string magic;
    int version = 0;
    if (!(input >> magic >> version) || magic != "DVE_RECENT_PROJECTS" || version != 1) {
        if (error) *error = "unsupported recent-project registry";
        return std::nullopt;
    }
    std::string remainder;
    std::getline(input, remainder);
    RecentProjectRegistry registry(capacity);
    std::vector<RecentProjectEntry> loaded;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        std::istringstream row(line);
        RecentProjectEntry entry;
        std::string genericPath;
        if (!(row >> std::quoted(entry.displayName) >> std::quoted(genericPath))) {
            if (error) *error = "malformed recent-project registry row";
            return std::nullopt;
        }
        row >> std::ws;
        if (!row.eof()) {
            if (error) *error = "unexpected data in recent-project registry row";
            return std::nullopt;
        }
        entry.projectFile = normalized_path(std::filesystem::path(genericPath));
        if (std::none_of(loaded.begin(), loaded.end(), [&](const RecentProjectEntry& prior) {
                return normalized_path(prior.projectFile) == entry.projectFile;
            }))
            loaded.push_back(std::move(entry));
        if (loaded.size() >= registry.capacity_) break;
    }
    registry.entries_ = std::move(loaded);
    return registry;
}

} // namespace dve::editor
