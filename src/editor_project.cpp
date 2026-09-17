#include "dve/editor_project.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "dve/editor_document.hpp"

namespace dve::editor {
namespace {

bool path_is_within(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code ec;
    const auto canonicalRoot = std::filesystem::weakly_canonical(root, ec);
    if (ec) return false;
    const auto canonicalPath = std::filesystem::weakly_canonical(path, ec);
    if (ec) return false;
    auto rootIt = canonicalRoot.begin();
    auto pathIt = canonicalPath.begin();
    for (; rootIt != canonicalRoot.end(); ++rootIt, ++pathIt) {
        if (pathIt == canonicalPath.end() || *rootIt != *pathIt) return false;
    }
    return true;
}

} // namespace

bool EditorProject::contains(const std::filesystem::path& path) const { return path_is_within(root, path); }
bool EditorProject::initialize(std::string* error) const {
    try {
        if (root.empty()) throw std::runtime_error("project root is empty");
        std::filesystem::create_directories(sources_dir());
        std::filesystem::create_directories(scenes_dir());
        std::filesystem::create_directories(imports_dir());
        std::filesystem::create_directories(materials_dir());
        std::filesystem::create_directories(cooked_dir());
        std::filesystem::create_directories(autosave_dir());
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}
bool EditorProject::save(std::string* error) const {
    try {
        if (!initialize(error)) return false;
        const auto temporary = root / "project.dveproject.tmp";
        const auto target = root / "project.dveproject";
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("could not create project file");
        out << "DVE_PROJECT 1\n" << "name " << std::quoted(name) << "\n" << "recent " << recentScenes.size() << "\n";
        for (const auto& scene : recentScenes) {
            if (!contains(root / scene)) throw std::runtime_error("recent scene escapes project root");
            out << std::quoted(scene.generic_string()) << "\n";
        }
        out.flush();
        if (!out) throw std::runtime_error("could not write project file");
        out.close();
        std::filesystem::rename(temporary, target);
        return true;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return false;
    }
}
std::optional<EditorProject> EditorProject::load(const std::filesystem::path& file, std::string* error) {
    try {
        std::ifstream in(file, std::ios::binary);
        if (!in) throw std::runtime_error("could not open project file");
        std::string magic, key, name;
        std::uint32_t version{};
        std::size_t recent{};
        in >> magic >> version;
        if (magic != "DVE_PROJECT" || version != 1) throw std::runtime_error("unsupported project format");
        in >> key >> std::quoted(name);
        if (key != "name") throw std::runtime_error("missing project name");
        in >> key >> recent;
        if (key != "recent") throw std::runtime_error("missing recent-scene count");
        EditorProject project;
        project.name = name;
        project.root = file.parent_path();
        for (std::size_t i = 0; i < recent; ++i) {
            std::string path; in >> std::quoted(path); project.recentScenes.emplace_back(path);
        }
        if (!in) throw std::runtime_error("malformed project file");
        for (const auto& scene : project.recentScenes)
            if (!project.contains(project.root / scene)) throw std::runtime_error("recent scene escapes project root");
        return project;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return std::nullopt;
    }
}

EditorAutosaveManager::EditorAutosaveManager(std::filesystem::path recoveryRoot) : root_(std::move(recoveryRoot)) {}
std::optional<std::filesystem::path> EditorAutosaveManager::save_recovery(
    const EditorDocument& document, std::string* error) const {
    try {
        std::filesystem::create_directories(root_);
        std::uint64_t sequence = 1;
        while (std::filesystem::exists(root_ / ("recovery_" + std::to_string(sequence) + ".dvescene"))) ++sequence;
        const auto path = root_ / ("recovery_" + std::to_string(sequence) + ".dvescene");
        EditorDocument copy = clone_editor_document(document);
        EditorSceneSaveResult saved = copy.save_transactional(path);
        if (!saved.success) throw std::runtime_error(saved.error);
        return path;
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return std::nullopt;
    }
}
std::vector<std::filesystem::path> EditorAutosaveManager::recoveries() const {
    std::vector<std::filesystem::path> result;
    std::error_code ec;
    if (!std::filesystem::exists(root_, ec)) return result;
    for (const auto& entry : std::filesystem::directory_iterator(root_, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".dvescene") result.push_back(entry.path());
    }
    std::sort(result.begin(), result.end());
    return result;
}

} // namespace dve::editor
