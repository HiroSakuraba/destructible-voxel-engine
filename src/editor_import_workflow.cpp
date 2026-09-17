#include "dve/editor_import_workflow.hpp"

#include <algorithm>
#include <cmath>

namespace dve::editor {

void ModelImportWorkflow::reset() {
    sourcePath_.clear();
    page_ = ImportWizardPage::Source;
    imported_ = {};
    settings_ = {};
    nodes_.clear();
    estimate_ = {};
    problems_.clear();
    cooked_ = {};
}

bool ModelImportWorkflow::choose_source(const std::filesystem::path& path, std::string* error) {
    reset();
    if (path.empty() || !std::filesystem::is_regular_file(path)) {
        if (error) *error = "source model does not exist";
        return false;
    }
    sourcePath_ = path;
    imported_ = import_model(path);
    if (!imported_.success) {
        if (error) {
            *error = "model import failed";
            for (const ImportDiagnostic& diagnostic : imported_.diagnostics)
                if (diagnostic.severity == ImportDiagnostic::Severity::Error) { *error = diagnostic.message; break; }
        }
        return false;
    }
    build_node_choices();
    build_problems();
    update_estimate();
    page_ = ImportWizardPage::ObjectStructure;
    return true;
}

void ModelImportWorkflow::build_node_choices() {
    nodes_.clear();
    for (std::size_t index = 0; index < imported_.scene.nodes.size(); ++index) {
        const ImportedNode& node = imported_.scene.nodes[index];
        if (!node.mesh) continue;
        std::string path = node.name.empty() ? "Node " + std::to_string(index) : node.name;
        auto parent = node.parent;
        while (parent) {
            const ImportedNode& p = imported_.scene.nodes[*parent];
            path = (p.name.empty() ? "Node " + std::to_string(*parent) : p.name) + "/" + path;
            parent = p.parent;
        }
        nodes_.push_back({node.name.empty() ? "Node " + std::to_string(index) : node.name, path});
    }
}

void ModelImportWorkflow::build_problems() {
    problems_.clear();
    for (const ImportDiagnostic& diagnostic : imported_.diagnostics) {
        EditorProblem problem;
        problem.severity = diagnostic.severity == ImportDiagnostic::Severity::Error ? ProblemSeverity::Error :
                           diagnostic.severity == ImportDiagnostic::Severity::Warning ? ProblemSeverity::Warning : ProblemSeverity::Information;
        problem.code = diagnostic.code;
        problem.summary = diagnostic.message;
        problem.details = diagnostic.message;
        if (diagnostic.code.find("BOUNDARY") != std::string::npos)
            problem.suggestedActions = {"Use shell mode", "Inspect open edges", "Continue intentionally"};
        else if (problem.severity == ProblemSeverity::Error)
            problem.suggestedActions = {"Select affected node", "Review import settings"};
        problems_.push_back(std::move(problem));
    }
}

void ModelImportWorkflow::update_estimate() {
    estimate_ = {};
    estimate_.nodes = static_cast<std::size_t>(std::count_if(nodes_.begin(), nodes_.end(), [](const ImportNodeChoice& node) { return node.included; }));
    for (const ImportedMesh& mesh : imported_.scene.meshes) estimate_.triangles += mesh.triangles.size();
    if (imported_.scene.meshes.empty()) return;
    Float3 minimum{INFINITY, INFINITY, INFINITY}, maximum{-INFINITY, -INFINITY, -INFINITY};
    for (const ImportedMesh& mesh : imported_.scene.meshes) for (const ImportedVertex& vertex : mesh.vertices) {
        minimum = {std::min(minimum.x, vertex.position.x), std::min(minimum.y, vertex.position.y), std::min(minimum.z, vertex.position.z)};
        maximum = {std::max(maximum.x, vertex.position.x), std::max(maximum.y, vertex.position.y), std::max(maximum.z, vertex.position.z)};
    }
    const float scale = 1.0F / std::max(0.0001F, settings_.voxelSizeMeters / static_cast<float>(settings_.supersampleFactor));
    const std::uint64_t x = static_cast<std::uint64_t>(std::max(1.0F, std::ceil((maximum.x - minimum.x) * scale) + 2.0F));
    const std::uint64_t y = static_cast<std::uint64_t>(std::max(1.0F, std::ceil((maximum.y - minimum.y) * scale) + 2.0F));
    const std::uint64_t z = static_cast<std::uint64_t>(std::max(1.0F, std::ceil((maximum.z - minimum.z) * scale) + 2.0F));
    estimate_.predictedWorkingVoxels = x > settings_.maximumWorkingVoxels / y ? settings_.maximumWorkingVoxels : std::min(settings_.maximumWorkingVoxels, x * y * z);
    estimate_.predictedBytes = estimate_.predictedWorkingVoxels * 8ULL;
}

bool ModelImportWorkflow::advance(std::string* error) {
    if (page_ == ImportWizardPage::Source) { if (error) *error = "choose a source model first"; return false; }
    if (page_ == ImportWizardPage::Complete) return true;
    if (page_ == ImportWizardPage::ObjectStructure && std::none_of(nodes_.begin(), nodes_.end(), [](const ImportNodeChoice& node) { return node.included; })) {
        if (error) *error = "at least one model node must be included";
        return false;
    }
    if (page_ == ImportWizardPage::VoxelSettings) {
        if (!(settings_.voxelSizeMeters > 0.0F) || (settings_.supersampleFactor != 1 && settings_.supersampleFactor != 2 && settings_.supersampleFactor != 4)) {
            if (error) *error = "invalid voxel settings";
            return false;
        }
        update_estimate();
    }
    page_ = static_cast<ImportWizardPage>(static_cast<int>(page_) + 1);
    return true;
}
bool ModelImportWorkflow::back() noexcept {
    if (page_ == ImportWizardPage::Source) return false;
    page_ = static_cast<ImportWizardPage>(static_cast<int>(page_) - 1);
    return true;
}

bool ModelImportWorkflow::cook(const std::filesystem::path& manifestPath, std::string* error) {
    if (sourcePath_.empty()) { if (error) *error = "no source model selected"; return false; }
    SceneCookSettings sceneSettings;
    sceneSettings.defaults = settings_;
    sceneSettings.splitByNode = true;
    for (const ImportNodeChoice& choice : nodes_) {
        NodeCookSettings settings;
        settings.ignore = !choice.included;
        settings.mode = choice.mode;
        settings.anchored = choice.anchored;
        settings.structural = choice.structural;
        settings.generateCollision = choice.generateCollision;
        sceneSettings.nodeOverrides[choice.name] = settings;
    }
    cooked_ = voxelize_scene_objects(imported_.scene, sceneSettings);
    if (!cooked_.success) { if (error) *error = "voxel cooking failed"; return false; }
    if (!write_dvox_scene_package(manifestPath, cooked_, error)) return false;
    page_ = ImportWizardPage::Complete;
    return true;
}

} // namespace dve::editor
