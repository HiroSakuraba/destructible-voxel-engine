#include "dve/asset_cooker.hpp"
#include "dve/mesh_remesh.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[nodiscard]] std::string require_value(int& index, int argc, char** argv, std::string_view option) {
    if (index + 1 >= argc) throw std::runtime_error("missing value for " + std::string(option));
    return argv[++index];
}

void append_mesh(const dve::ImportedMesh& mesh, const dve::Matrix4& transform,
                 std::vector<dve::Float3>& positions,
                 std::vector<std::uint32_t>& indices) {
    if (positions.size() > UINT32_MAX - mesh.vertices.size())
        throw std::runtime_error("scene vertex count exceeds 32-bit remesher index limits");
    const auto base = static_cast<std::uint32_t>(positions.size());
    for (const dve::ImportedVertex& vertex : mesh.vertices)
        positions.push_back(dve::transform_point(transform, vertex.position));
    for (const dve::ImportedTriangle& triangle : mesh.triangles) {
        for (const std::uint32_t index : triangle.indices) {
            if (index >= mesh.vertices.size()) throw std::runtime_error("imported triangle index is invalid");
            indices.push_back(base + index);
        }
    }
}

void write_obj(const std::filesystem::path& path, const dve::RemeshedTriangleMesh& mesh) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("could not open output OBJ");
    output << "# DVE voxel-aware remesh; materials and UVs are not preserved\n";
    output << std::fixed << std::setprecision(7);
    for (const dve::Float3 position : mesh.positions)
        output << "v " << position.x << ' ' << position.y << ' ' << position.z << '\n';
    for (const dve::Float3 normal : mesh.cornerNormals)
        output << "vn " << normal.x << ' ' << normal.y << ' ' << normal.z << '\n';
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3U) {
        output << "f";
        for (std::size_t corner = 0; corner < 3U; ++corner) {
            const std::size_t normalIndex = i + corner + 1U;
            output << ' ' << (static_cast<std::uint64_t>(mesh.indices[i + corner]) + 1U)
                   << "//" << normalIndex;
        }
        output << '\n';
    }
    output.flush();
    if (!output) throw std::runtime_error("failed while writing output OBJ");
}

void usage() {
    std::cout << "dve_remesh_mesh <source.gltf|source.glb|source.obj> --output <mesh.obj> [options]\n"
                 "  --resolution <4..256>       Voxel grid resolution (default 64)\n"
                 "  --shell                    Keep a two-sided shell instead of filling closed interiors\n"
                 "  --crease-angle <degrees>   Normal crease cutoff (default 60)\n"
                 "  --normal-smoothing <0..5>  Normal post-smoothing (default 0)\n"
                 "  --max-triangles <count>    Output allocation limit (default 4000000)\n"
                 "  --non-strict               Allow importer warnings\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) { usage(); return 2; }
        const std::filesystem::path source = argv[1];
        std::filesystem::path outputPath;
        dve::ModelImportOptions importOptions;
        dve::MeshRemeshOptions options;

        for (int index = 2; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (argument == "--output") outputPath = require_value(index, argc, argv, argument);
            else if (argument == "--resolution") options.resolution = static_cast<std::uint32_t>(
                std::stoul(require_value(index, argc, argv, argument)));
            else if (argument == "--shell") options.shell = true;
            else if (argument == "--crease-angle") {
                constexpr float pi = 3.14159265358979323846F;
                options.creaseAngleRadians = std::stof(require_value(index, argc, argv, argument)) * pi / 180.0F;
            } else if (argument == "--normal-smoothing")
                options.normalSmoothing = std::stof(require_value(index, argc, argv, argument));
            else if (argument == "--max-triangles")
                options.maximumOutputTriangles = std::stoull(require_value(index, argc, argv, argument));
            else if (argument == "--non-strict") importOptions.strict = false;
            else if (argument == "--help" || argument == "-h") { usage(); return 0; }
            else throw std::runtime_error("unknown option: " + std::string(argument));
        }
        if (outputPath.empty()) throw std::runtime_error("--output is required");
        if (outputPath.extension() != ".obj") throw std::runtime_error("output path must end in .obj");

        dve::ImportedModelResult imported = dve::import_model(source, importOptions);
        for (const dve::ImportDiagnostic& diagnostic : imported.diagnostics) {
            std::cerr << dve::to_string(diagnostic.severity) << " [" << diagnostic.code << "] "
                      << diagnostic.message << '\n';
        }
        if (!imported.success) throw std::runtime_error("source model import failed");

        std::vector<dve::Float3> positions;
        std::vector<std::uint32_t> indices;
        bool instanced = false;
        for (const dve::ImportedNode& node : imported.scene.nodes) {
            if (!node.mesh) continue;
            if (*node.mesh >= imported.scene.meshes.size()) throw std::runtime_error("imported node mesh index is invalid");
            append_mesh(imported.scene.meshes[*node.mesh], node.worldTransform, positions, indices);
            instanced = true;
        }
        if (!instanced) {
            for (std::size_t i = 0; i < imported.scene.meshes.size(); ++i)
                append_mesh(imported.scene.meshes[i], dve::Matrix4::identity(), positions, indices);
        }
        dve::RemeshedTriangleMesh remeshed;
        std::string error;
        if (!dve::remesh_triangle_mesh(positions, indices, options, remeshed, &error))
            throw std::runtime_error("remeshing failed: " + error);
        write_obj(outputPath, remeshed);

        std::cout << "Remeshed " << source << " -> " << outputPath << " at resolution "
                  << options.resolution << " (" << positions.size() << " input vertices, "
                  << indices.size() / 3U << " input triangles; " << remeshed.positions.size()
                  << " output vertices, " << remeshed.indices.size() / 3U << " output triangles).\n"
                  << "This geometry-only output does not preserve source materials, UVs, or tangents.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_remesh_mesh: " << exception.what() << '\n';
        return 2;
    }
}
