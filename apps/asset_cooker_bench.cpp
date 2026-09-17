#include "dve/asset_cooker.hpp"
#include "dve/dvox.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

dve::ImportedScene make_box(float minimum, float maximum) {
    dve::ImportedScene scene;
    dve::ImportedMaterial material;
    material.name = "BenchmarkConcrete";
    material.metallicFactor = 0.0F;
    material.roughnessFactor = 0.85F;
    scene.materials.push_back(material);
    dve::ImportedMesh mesh;
    mesh.name = "BenchmarkBox";
    const dve::Float3 positions[] = {
        {minimum, minimum, minimum}, {maximum, minimum, minimum}, {maximum, maximum, minimum}, {minimum, maximum, minimum},
        {minimum, minimum, maximum}, {maximum, minimum, maximum}, {maximum, maximum, maximum}, {minimum, maximum, maximum},
    };
    for (dve::Float3 position : positions) {
        dve::ImportedVertex vertex;
        vertex.position = position;
        mesh.vertices.push_back(vertex);
    }
    const std::uint32_t faces[][3] = {
        {0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
        {0, 1, 5}, {0, 5, 4}, {3, 7, 6}, {3, 6, 2},
        {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5},
    };
    for (const auto& face : faces) mesh.triangles.push_back({{face[0], face[1], face[2]}, 0, 0});
    scene.meshes.push_back(std::move(mesh));
    dve::ImportedNode node;
    node.mesh = 0;
    scene.nodes.push_back(node);
    scene.roots.push_back(0);
    return scene;
}

dve::ImportedScene make_textured_wall(float sizeMeters, std::uint32_t imageSize) {
    dve::ImportedScene scene;
    scene.name = "TexturedWallBenchmark";

    dve::ImportedImage image;
    image.name = "ProceduralTexture";
    image.mimeType = "image/raw-rgba8";
    image.width = imageSize;
    image.height = imageSize;
    image.rgba8.resize(static_cast<std::size_t>(imageSize) * imageSize * 4U);
    for (std::uint32_t y = 0; y < imageSize; ++y) {
        for (std::uint32_t x = 0; x < imageSize; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * imageSize + x) * 4U;
            image.rgba8[offset] = static_cast<std::uint8_t>((x * 255U) / std::max(1U, imageSize - 1U));
            image.rgba8[offset + 1U] = static_cast<std::uint8_t>((y * 255U) / std::max(1U, imageSize - 1U));
            image.rgba8[offset + 2U] = static_cast<std::uint8_t>(((x ^ y) * 255U) / std::max(1U, imageSize - 1U));
            image.rgba8[offset + 3U] = 255U;
        }
    }
    scene.images.push_back(std::move(image));

    dve::ImportedSampler sampler;
    sampler.minFilter = dve::ImportedTextureFilter::Linear;
    sampler.magFilter = dve::ImportedTextureFilter::Linear;
    sampler.wrapS = dve::ImportedWrapMode::ClampToEdge;
    sampler.wrapT = dve::ImportedWrapMode::ClampToEdge;
    scene.samplers.push_back(sampler);

    dve::ImportedTexture texture;
    texture.name = "WallTexture";
    texture.imageIndex = 0;
    texture.samplerIndex = 0;
    scene.textures.push_back(texture);

    dve::ImportedMaterial material;
    material.name = "TexturedPaint";
    material.baseColorTexture = 0;
    material.metallicFactor = 0.0F;
    material.roughnessFactor = 0.7F;
    scene.materials.push_back(material);

    dve::ImportedMesh mesh;
    mesh.name = "Wall";
    const dve::Float3 positions[] = {
        {0.01F, 0.01F, 0.025F}, {sizeMeters - 0.01F, 0.01F, 0.025F},
        {sizeMeters - 0.01F, sizeMeters - 0.01F, 0.025F}, {0.01F, sizeMeters - 0.01F, 0.025F},
    };
    const dve::Float2 uvs[] = {{0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F}, {0.0F, 1.0F}};
    for (std::size_t index = 0; index < 4; ++index) {
        dve::ImportedVertex vertex;
        vertex.position = positions[index];
        vertex.texcoord = uvs[index];
        mesh.vertices.push_back(vertex);
    }
    mesh.triangles.push_back({{0, 1, 2}, 0, 0});
    mesh.triangles.push_back({{0, 2, 3}, 0, 0});
    scene.meshes.push_back(std::move(mesh));
    dve::ImportedNode node;
    node.mesh = 0;
    scene.nodes.push_back(node);
    scene.roots.push_back(0);
    return scene;
}

template <class Fn>
double time_ms(Fn&& function) {
    const auto begin = std::chrono::steady_clock::now();
    function();
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

template <class Fn>
double median_ms(std::size_t iterations, Fn&& function) {
    std::vector<double> samples;
    samples.reserve(iterations);
    for (std::size_t i = 0; i < iterations; ++i) samples.push_back(time_ms(function));
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2U];
}

void require_success(const dve::CookedVoxelAsset& asset) {
    for (const auto& diagnostic : asset.diagnostics) {
        if (diagnostic.severity == dve::ImportDiagnostic::Severity::Error) {
            throw std::runtime_error(diagnostic.message);
        }
    }
}

std::uintmax_t roundtrip_asset(
    const std::filesystem::path& path,
    const dve::CookedVoxelAsset& asset,
    double& writeMs,
    double& readMs) {
    std::string error;
    writeMs = median_ms(7, [&] {
        if (!dve::write_dvox(path, asset, {}, &error)) throw std::runtime_error(error);
    });
    dve::DvoxReadResult loaded;
    readMs = median_ms(7, [&] {
        loaded = dve::read_dvox(path);
        if (!loaded.success) throw std::runtime_error(loaded.error);
    });
    return std::filesystem::file_size(path);
}

} // namespace

int main() {
    try {
        dve::VoxelizeSettings solidSettings;
        solidSettings.mode = dve::VoxelizationMode::Solid;
        solidSettings.voxelSizeMeters = 0.10F;
        solidSettings.maximumWorkingVoxels = 16ULL * 1024ULL * 1024ULL;
        dve::CookedVoxelAsset solid;
        const dve::ImportedScene box = make_box(0.01F, 5.99F);
        const double solidCookMs = median_ms(7, [&] { solid = dve::voxelize_scene(box, solidSettings); });
        require_success(solid);

        dve::VoxelizeSettings textureSettings;
        textureSettings.mode = dve::VoxelizationMode::SurfaceOnly;
        textureSettings.voxelSizeMeters = 0.05F;
        textureSettings.maximumPaletteMaterials = 64;
        textureSettings.maximumWorkingVoxels = 16ULL * 1024ULL * 1024ULL;
        dve::CookedVoxelAsset textured;
        const dve::ImportedScene wall = make_textured_wall(8.0F, 256U);
        const double texturedCookMs = median_ms(7, [&] { textured = dve::voxelize_scene(wall, textureSettings); });
        require_success(textured);

        const std::filesystem::path temp = std::filesystem::temp_directory_path();
        const std::filesystem::path solidPath = temp / "dve_asset_cooker_bench_solid.dvox";
        const std::filesystem::path texturedPath = temp / "dve_asset_cooker_bench_textured.dvox";
        double solidWriteMs = 0.0;
        double solidReadMs = 0.0;
        double texturedWriteMs = 0.0;
        double texturedReadMs = 0.0;
        const std::uintmax_t solidBytes = roundtrip_asset(solidPath, solid, solidWriteMs, solidReadMs);
        const std::uintmax_t texturedBytes = roundtrip_asset(texturedPath, textured, texturedWriteMs, texturedReadMs);
        std::filesystem::remove(solidPath);
        std::filesystem::remove(texturedPath);

        std::cout << "{\n"
                  << "  \"iterations\": 7,\n"
                  << "  \"solid_box\": {\n"
                  << "    \"size_meters\": 5.98,\n"
                  << "    \"voxel_size_meters\": 0.1,\n"
                  << "    \"triangles\": " << solid.stats.sourceTriangles << ",\n"
                  << "    \"voxels\": " << solid.object.occupied_voxel_count() << ",\n"
                  << "    \"bricks\": " << solid.object.brick_count() << ",\n"
                  << "    \"cook_median_ms\": " << solidCookMs << ",\n"
                  << "    \"write_median_ms\": " << solidWriteMs << ",\n"
                  << "    \"read_median_ms\": " << solidReadMs << ",\n"
                  << "    \"dvox_bytes\": " << solidBytes << "\n"
                  << "  },\n"
                  << "  \"textured_wall\": {\n"
                  << "    \"size_meters\": 8.0,\n"
                  << "    \"voxel_size_meters\": 0.05,\n"
                  << "    \"image_width\": 256,\n"
                  << "    \"image_height\": 256,\n"
                  << "    \"triangles\": " << textured.stats.sourceTriangles << ",\n"
                  << "    \"texture_samples\": " << textured.stats.textureSamples << ",\n"
                  << "    \"unique_material_samples\": " << textured.stats.uniqueMaterialSamples << ",\n"
                  << "    \"palette_materials\": " << textured.stats.paletteMaterials << ",\n"
                  << "    \"voxels\": " << textured.object.occupied_voxel_count() << ",\n"
                  << "    \"bricks\": " << textured.object.brick_count() << ",\n"
                  << "    \"cook_median_ms\": " << texturedCookMs << ",\n"
                  << "    \"write_median_ms\": " << texturedWriteMs << ",\n"
                  << "    \"read_median_ms\": " << texturedReadMs << ",\n"
                  << "    \"dvox_bytes\": " << texturedBytes << "\n"
                  << "  }\n"
                  << "}\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_asset_cooker_bench: " << exception.what() << '\n';
        return 1;
    }
}
