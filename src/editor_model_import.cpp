#include "dve/editor_model_import.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

namespace dve::editor {

std::string_view import_mode_label(VoxelizationMode mode) noexcept {
    switch (mode) {
        case VoxelizationMode::Solid: return "Solid";
        case VoxelizationMode::Shell: return "Shell";
        case VoxelizationMode::SurfaceOnly: return "Surface only";
    }
    return "Solid";
}

std::vector<EditorImportNode> list_import_nodes(const ImportedScene& scene) {
    std::vector<EditorImportNode> nodes;
    for (std::size_t index = 0; index < scene.nodes.size(); ++index) {
        const ImportedNode& node = scene.nodes[index];
        if (!node.mesh || *node.mesh >= scene.meshes.size()) continue;
        const std::string name = node.name.empty() ? "Node " + std::to_string(index) : node.name;
        std::string path = name;
        std::size_t guard = 0;
        for (auto parent = node.parent; parent && *parent < scene.nodes.size() && guard < 64; ++guard) {
            const ImportedNode& p = scene.nodes[*parent];
            path = (p.name.empty() ? "Node " + std::to_string(*parent) : p.name) + "/" + path;
            parent = p.parent;
        }
        nodes.push_back({node.name, std::move(path), scene.meshes[*node.mesh].triangles.size()});
    }
    return nodes;
}

std::string validate_import_settings(const EditorImportSettings& settings) {
    if (!(settings.voxelSizeMeters >= kMinImportVoxelSize && settings.voxelSizeMeters <= kMaxImportVoxelSize))
        return "Voxel size must be between 0.005 m and 10 m";
    if (!(settings.scale >= kMinImportScale && settings.scale <= kMaxImportScale))
        return "Scale must be between 0.001 and 1000";
    return {};
}

namespace {
std::string format_count(std::uint64_t value) {
    std::string digits = std::to_string(value);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) digits.insert(static_cast<std::size_t>(i), ",");
    return digits;
}
} // namespace

EditorImportEstimate estimate_import(const ImportedScene& scene, const EditorImportSettings& settings) {
    EditorImportEstimate estimate;
    if (std::string invalid = validate_import_settings(settings); !invalid.empty()) {
        estimate.refusal = std::move(invalid);
        return estimate;
    }
    const float cell = settings.voxelSizeMeters;
    for (const ImportedNode& node : scene.nodes) {
        if (!node.mesh || *node.mesh >= scene.meshes.size()) continue;
        ++estimate.totalNodes;
        if (settings.excludedNodes.contains(node.name)) continue;
        ++estimate.includedNodes;
        const ImportedMesh& mesh = scene.meshes[*node.mesh];
        estimate.triangles += mesh.triangles.size();
        if (mesh.vertices.empty()) continue;
        Float3 minimum{INFINITY, INFINITY, INFINITY};
        Float3 maximum{-INFINITY, -INFINITY, -INFINITY};
        for (const ImportedVertex& vertex : mesh.vertices) {
            const Float3 p = transform_point(node.worldTransform, vertex.position);
            minimum = {std::min(minimum.x, p.x), std::min(minimum.y, p.y), std::min(minimum.z, p.z)};
            maximum = {std::max(maximum.x, p.x), std::max(maximum.y, p.y), std::max(maximum.z, p.z)};
        }
        const auto cells = [&](float lo, float hi) {
            const double extent = static_cast<double>(hi - lo) * static_cast<double>(settings.scale);
            return std::max(1.0, std::ceil(extent / static_cast<double>(cell)) + 1.0);
        };
        const double count = cells(minimum.x, maximum.x) * cells(minimum.y, maximum.y) * cells(minimum.z, maximum.z);
        estimate.voxels = count >= 1.0e18 ? ~std::uint64_t{0} >> 1
                                          : std::min<std::uint64_t>(estimate.voxels + static_cast<std::uint64_t>(count), ~std::uint64_t{0} >> 1);
    }
    if (estimate.totalNodes == 0) estimate.refusal = "The model has no meshes to import";
    else if (estimate.includedNodes == 0) estimate.refusal = "Every mesh is left out: include at least one";
    else if (estimate.voxels > kMaxImportVoxels) {
        char size[32];
        std::snprintf(size, sizeof size, "%.3g m", static_cast<double>(settings.voxelSizeMeters));
        estimate.refusal = "Too large to import: about " + format_count(estimate.voxels) + " voxels at " + size +
                           "; the limit is " + format_count(kMaxImportVoxels) +
                           ". Use a larger voxel size, a smaller scale, or leave meshes out";
    }
    return estimate;
}

ImportedScene scaled_import_scene(const ImportedScene& scene, float scale) {
    ImportedScene copy = scene;
    if (scale == 1.0F) return copy;
    // Uniform scale about the origin: the cooker bakes each node's world scale into its mesh,
    // so scaling the rows of every world (and local) transform scales positions and sizes.
    for (ImportedNode& node : copy.nodes)
        for (Matrix4* matrix : {&node.worldTransform, &node.localTransform})
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 3; ++row) matrix->values[static_cast<std::size_t>(column * 4 + row)] *= scale;
    return copy;
}

SceneCookSettings import_cook_settings(const ImportedScene& scene, const EditorImportSettings& settings) {
    SceneCookSettings cook;
    cook.defaults.voxelSizeMeters = settings.voxelSizeMeters;
    cook.defaults.mode = settings.mode;
    cook.defaults.maximumWorkingVoxels = kMaxImportVoxels;
    cook.splitByNode = true;
    for (const ImportedNode& node : scene.nodes)
        if (node.mesh && settings.excludedNodes.contains(node.name)) cook.nodeOverrides[node.name].ignore = true;
    return cook;
}

Component make_import_settings_component(const EditorImportSettings& settings, ComponentId id) {
    Component component;
    component.id = id;
    component.type = std::string(kImportSettingsComponent);
    component.properties.emplace("voxel_size", static_cast<double>(settings.voxelSizeMeters));
    component.properties.emplace("scale", static_cast<double>(settings.scale));
    component.properties.emplace("mode", settings.mode == VoxelizationMode::Shell ? std::string("shell")
                                         : settings.mode == VoxelizationMode::SurfaceOnly ? std::string("surface")
                                                                                          : std::string("solid"));
    std::string excluded;
    for (const std::string& name : settings.excludedNodes) {
        if (!excluded.empty()) excluded += '\n';
        excluded += name;
    }
    component.properties.emplace("excluded", excluded);
    return component;
}

std::optional<EditorImportSettings> read_import_settings(const EditorObject& object) {
    const auto found = std::find_if(object.components.begin(), object.components.end(),
                                    [](const Component& c) { return c.type == kImportSettingsComponent; });
    if (found == object.components.end()) return std::nullopt;
    EditorImportSettings settings;
    const auto number = [&](std::string_view name, double fallback) {
        const auto it = found->properties.find(name);
        if (it == found->properties.end()) return fallback;
        if (const auto* d = std::get_if<double>(&it->second)) return *d;
        if (const auto* i = std::get_if<std::int64_t>(&it->second)) return static_cast<double>(*i);
        return fallback;
    };
    settings.voxelSizeMeters = static_cast<float>(std::clamp(number("voxel_size", settings.voxelSizeMeters),
                                                             double(kMinImportVoxelSize), double(kMaxImportVoxelSize)));
    settings.scale = static_cast<float>(std::clamp(number("scale", settings.scale), double(kMinImportScale), double(kMaxImportScale)));
    if (const auto it = found->properties.find("mode"); it != found->properties.end())
        if (const auto* text = std::get_if<std::string>(&it->second))
            settings.mode = *text == "shell" ? VoxelizationMode::Shell
                            : *text == "surface" ? VoxelizationMode::SurfaceOnly : VoxelizationMode::Solid;
    if (const auto it = found->properties.find("excluded"); it != found->properties.end())
        if (const auto* text = std::get_if<std::string>(&it->second)) {
            std::size_t start = 0;
            while (start <= text->size()) {
                const std::size_t end = std::min(text->find('\n', start), text->size());
                if (end > start) settings.excludedNodes.insert(text->substr(start, end - start));
                start = end + 1;
            }
        }
    return settings;
}

bool is_model_source(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return extension == ".obj" || extension == ".gltf" || extension == ".glb";
}

} // namespace dve::editor
