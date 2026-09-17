#include "dve/asset_cooker.hpp"
#include "dve/dvox.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void append_float_le(std::vector<std::byte>& bytes, float value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::byte>((bits >> shift) & 0xFFU));
    }
}

void append_u16_le(std::vector<std::byte>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>(value & 0xFFU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}


void append_u32_le(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(static_cast<bool>(input), "unable to open test file");
    const std::streamoff size = input.tellg();
    require(size >= 0, "unable to determine test file size");
    input.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) require(static_cast<bool>(input.read(reinterpret_cast<char*>(bytes.data()), size)),
                                "unable to read test file");
    return bytes;
}

void test_dvox_all_encodings(const std::filesystem::path& temp) {
    dve::CookedVoxelAsset asset(99);
    asset.voxelSizeMeters = 0.25F;
    for (int i = 0; i <= 24; ++i) {
        dve::VoxelMaterialDefinition material;
        material.name = i == 0 ? "Air" : "Material_" + std::to_string(i);
        material.structural = i != 0;
        material.densityKilogramsPerCubicMeter = i == 0 ? 0.0F : 1000.0F + static_cast<float>(i);
        asset.materials.push_back(material);
    }
    asset.materials[1].shadingModel = dve::MaterialShadingModel::ClearCoat;
    asset.materials[1].specular = 0.65F;
    asset.materials[1].clearCoat = 0.9F;
    asset.materials[1].clearCoatRoughness = 0.07F;
    asset.materials[1].foliageColor = {0.2F, 0.7F, 0.1F};
    asset.materials[1].foliageTransmittance = 0.6F;
    asset.materials[1].foliageWrap = 0.4F;
    asset.materials[1].layers.push_back({2, 0.25F, dve::MaterialLayerBlendMode::Multiply, true});
    asset.object.fill_brick({0, 0, 0}, 1);
    asset.object.set_voxel({8, 0, 0}, 2);
    asset.object.set_voxel({9, 0, 0}, 2);
    for (int x = 16; x < 24; ++x) asset.object.set_voxel({x, 0, 0}, static_cast<dve::MaterialId>(3 + (x % 3)));
    for (int i = 0; i < 20; ++i) asset.object.set_voxel({24 + (i % 8), (i / 8) % 8, i / 64}, static_cast<dve::MaterialId>(i + 1));
    require(asset.object.find_brick({0, 0, 0})->encoding() == dve::BrickEncoding::UniformSolid,
            "uniform encoding fixture failed");
    require(asset.object.find_brick({1, 0, 0})->encoding() == dve::BrickEncoding::MaskUniform,
            "mask uniform encoding fixture failed");
    require(asset.object.find_brick({2, 0, 0})->encoding() == dve::BrickEncoding::LocalPalette4,
            "local palette encoding fixture failed");
    require(asset.object.find_brick({3, 0, 0})->encoding() == dve::BrickEncoding::Palette8,
            "palette8 encoding fixture failed");
    const std::filesystem::path path = temp / "all_encodings.dvox";
    std::string error;
    require(dve::write_dvox(path, asset, {}, &error), error.c_str());
    dve::DvoxReadResult loaded = dve::read_dvox(path);
    require(loaded.success, loaded.error.c_str());
    require(loaded.asset.object.occupied_voxel_count() == asset.object.occupied_voxel_count(),
            "all-encoding DVOX occupancy mismatch");
    const auto& loadedMaterial = loaded.asset.materials.at(1);
    require(loadedMaterial.shadingModel == dve::MaterialShadingModel::ClearCoat,
            "DVOX shading model did not round trip");
    require(loadedMaterial.specular == 0.65F && loadedMaterial.clearCoat == 0.9F &&
            loadedMaterial.clearCoatRoughness == 0.07F, "DVOX clear-coat fields did not round trip");
    require(loadedMaterial.layers.size() == 1 && loadedMaterial.layers[0].sourceMaterial == 2 &&
            loadedMaterial.layers[0].blendMode == dve::MaterialLayerBlendMode::Multiply,
            "DVOX material layers did not round trip");
    for (int z = 0; z < 8; ++z) {
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 32; ++x) {
                require(loaded.asset.object.material_at({x, y, z}) == asset.object.material_at({x, y, z}),
                        "all-encoding DVOX material mismatch");
            }
        }
    }
}

dve::ImportedScene make_cube_scene() {
    dve::ImportedScene scene;
    scene.name = "Cube";
    dve::ImportedMaterial material;
    material.name = "Concrete";
    material.metallicFactor = 0.0F;
    material.roughnessFactor = 0.8F;
    scene.materials.push_back(material);

    dve::ImportedMesh mesh;
    mesh.name = "CubeMesh";
    const dve::Float3 positions[] = {
        {0.1F, 0.1F, 0.1F}, {1.9F, 0.1F, 0.1F}, {1.9F, 1.9F, 0.1F}, {0.1F, 1.9F, 0.1F},
        {0.1F, 0.1F, 1.9F}, {1.9F, 0.1F, 1.9F}, {1.9F, 1.9F, 1.9F}, {0.1F, 1.9F, 1.9F},
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
    node.name = "CubeNode";
    node.mesh = 0;
    scene.nodes.push_back(node);
    scene.roots.push_back(0);
    return scene;
}

void test_cube_voxelization_and_dvox_roundtrip(const std::filesystem::path& temp) {
    dve::VoxelizeSettings settings;
    settings.voxelSizeMeters = 1.0F;
    settings.mode = dve::VoxelizationMode::Solid;
    settings.objectId = 77;
    dve::CookedVoxelAsset asset = dve::voxelize_scene(make_cube_scene(), settings);
    require(asset.object.validate(), "voxelized cube object must validate");
    require(asset.stats.sourceTriangles == 12, "cube source triangle count mismatch");
    require(asset.stats.boundaryEdges == 0, "closed cube must have no boundary edges");
    require(asset.stats.nonManifoldEdges == 0, "closed cube must have no non-manifold edges");
    require(asset.stats.meshIslands == 1, "closed cube must have one mesh island");
    require(asset.object.occupied_voxel_count() == 8, "1.8m cube at 1m resolution should occupy 8 voxels");
    require(asset.object.brick_count() == 1, "cube should fit in one brick");
    require(asset.materials.size() == 2, "air plus one source material expected");

    dve::ImportedScene aligned = make_cube_scene();
    for (auto& vertex : aligned.meshes[0].vertices) {
        vertex.position.x = vertex.position.x < 1.0F ? 0.0F : 2.0F;
        vertex.position.y = vertex.position.y < 1.0F ? 0.0F : 2.0F;
        vertex.position.z = vertex.position.z < 1.0F ? 0.0F : 2.0F;
    }
    const dve::CookedVoxelAsset alignedAsset = dve::voxelize_scene(aligned, settings);
    require(alignedAsset.object.occupied_voxel_count() == 8,
            "grid-aligned two-metre cube must not grow an extra boundary layer");

    const std::filesystem::path path = temp / "cube_roundtrip.dvox";
    const std::filesystem::path secondPath = temp / "cube_roundtrip_second.dvox";
    std::string error;
    require(dve::write_dvox(path, asset, {}, &error), error.c_str());
    require(dve::write_dvox(secondPath, asset, {}, &error), error.c_str());
    require(read_bytes(path) == read_bytes(secondPath), "identical assets must produce byte-identical DVOX files");
    dve::DvoxReadResult read = dve::read_dvox(path);
    require(read.success, read.error.c_str());
    require(read.asset.object.id() == 77, "DVOX object ID mismatch");
    require(read.asset.object.occupied_voxel_count() == asset.object.occupied_voxel_count(), "DVOX occupancy mismatch");
    require(read.asset.materials.size() == asset.materials.size(), "DVOX material count mismatch");
    for (int z = -1; z <= 2; ++z) {
        for (int y = -1; y <= 2; ++y) {
            for (int x = -1; x <= 2; ++x) {
                require(read.asset.object.material_at({x, y, z}) == asset.object.material_at({x, y, z}),
                        "DVOX voxel material mismatch");
            }
        }
    }
}

void test_dvox_full_content_hash_and_size_limit(const std::filesystem::path& temp) {
    dve::CookedVoxelAsset asset(1234);
    asset.voxelSizeMeters = 0.125F;
    asset.materials.resize(2);
    asset.materials[0].name = "Air";
    asset.materials[0].densityKilogramsPerCubicMeter = 0.0F;
    asset.materials[0].structural = false;
    asset.materials[1].name = "DensePaintedSteel";
    asset.materials[1].baseColor = {0.2F, 0.4F, 0.8F, 1.0F};
    asset.materials[1].densityKilogramsPerCubicMeter = 7850.0F;
    asset.object.set_voxel({0, 0, 0}, 1);

    const std::filesystem::path path = temp / "full_content_hash.dvox";
    std::string error;
    require(dve::write_dvox(path, asset, {}, &error), error.c_str());
    const std::uint64_t fileSize = std::filesystem::file_size(path);
    require(dve::read_dvox(path, fileSize).success, "DVOX should load at its exact size limit");
    const dve::DvoxReadResult limited = dve::read_dvox(path, fileSize - 1U);
    require(!limited.success && limited.error.find("size limit") != std::string::npos,
            "DVOX read must enforce the configured byte limit");

    std::vector<std::byte> bytes = read_bytes(path);
    require(bytes.size() > 64U, "DVOX fixture must contain a material section");
    bytes[64] ^= std::byte{0x01};
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "unable to reopen DVOX corruption fixture");
        output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    const dve::DvoxReadResult corrupted = dve::read_dvox(path);
    require(!corrupted.success && corrupted.error.find("full-content hash mismatch") != std::string::npos,
            "DVOX v1.1 must detect material-section corruption");
}

void test_surface_and_shell_modes() {
    dve::ImportedScene scene;
    dve::ImportedMaterial material;
    material.name = "Sheet";
    scene.materials.push_back(material);
    dve::ImportedMesh mesh;
    mesh.name = "Plane";
    for (dve::Float3 position : {dve::Float3{0.1F, 0.1F, 0.5F}, dve::Float3{1.9F, 0.1F, 0.5F},
                                 dve::Float3{1.9F, 1.9F, 0.5F}, dve::Float3{0.1F, 1.9F, 0.5F}}) {
        dve::ImportedVertex vertex;
        vertex.position = position;
        mesh.vertices.push_back(vertex);
    }
    mesh.triangles.push_back({{0, 1, 2}, 0, 0});
    mesh.triangles.push_back({{0, 2, 3}, 0, 0});
    scene.meshes.push_back(std::move(mesh));
    dve::ImportedNode node;
    node.mesh = 0;
    scene.nodes.push_back(node);
    scene.roots.push_back(0);

    dve::VoxelizeSettings surfaceSettings;
    surfaceSettings.voxelSizeMeters = 1.0F;
    surfaceSettings.mode = dve::VoxelizationMode::SurfaceOnly;
    const dve::CookedVoxelAsset surface = dve::voxelize_scene(scene, surfaceSettings);
    require(surface.object.occupied_voxel_count() == 4, "surface plane should occupy four voxels");
    require(surface.stats.boundaryEdges == 4, "open plane should report four boundary edges");
    require(surface.stats.meshIslands == 1, "open plane should report one mesh island");

    dve::VoxelizeSettings shellSettings = surfaceSettings;
    shellSettings.mode = dve::VoxelizationMode::Shell;
    shellSettings.shellThicknessVoxels = 2;
    const dve::CookedVoxelAsset shell = dve::voxelize_scene(scene, shellSettings);
    require(shell.object.occupied_voxel_count() > surface.object.occupied_voxel_count(),
            "thickened shell should contain more voxels than surface mode");
}

void test_gltf_import(const std::filesystem::path& temp) {
    const std::filesystem::path binPath = temp / "triangle.bin";
    const std::filesystem::path gltfPath = temp / "triangle.gltf";
    std::vector<std::byte> buffer;
    for (float value : {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F}) append_float_le(buffer, value);
    append_u16_le(buffer, 0);
    append_u16_le(buffer, 1);
    append_u16_le(buffer, 2);
    {
        std::ofstream output(binPath, std::ios::binary);
        output.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    }
    {
        std::ofstream output(gltfPath);
        output << R"({
  "asset":{"version":"2.0"},
  "buffers":[{"uri":"triangle.bin","byteLength":42}],
  "bufferViews":[
    {"buffer":0,"byteOffset":0,"byteLength":36},
    {"buffer":0,"byteOffset":36,"byteLength":6}
  ],
  "accessors":[
    {"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
    {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}
  ],
  "materials":[{"name":"Red","pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1],"metallicFactor":0,"roughnessFactor":1}}],
  "meshes":[{"name":"Triangle","primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]}],
  "nodes":[{"name":"Translated","mesh":0,"translation":[1,2,3]}],
  "scenes":[{"nodes":[0]}],
  "scene":0
})";
    }
    const dve::ImportedModelResult imported = dve::import_model(gltfPath);
    require(imported.success, "minimal glTF must import");
    require(imported.scene.meshes.size() == 1, "glTF mesh count mismatch");
    require(imported.scene.meshes[0].triangles.size() == 1, "glTF triangle count mismatch");
    require(imported.scene.nodes.size() == 1, "glTF node count mismatch");
    const dve::Float3 world = dve::transform_point(imported.scene.nodes[0].worldTransform,
                                                    imported.scene.meshes[0].vertices[0].position);
    require(world.x == 1.0F && world.y == 2.0F && world.z == 3.0F, "glTF node transform mismatch");
    require(imported.scene.materials[0].baseColorFactor.x == 1.0F &&
            imported.scene.materials[0].baseColorFactor.y == 0.0F, "glTF material factor mismatch");
}


void test_glb_import(const std::filesystem::path& temp) {
    std::vector<std::byte> binary;
    for (float value : {0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F}) append_float_le(binary, value);
    append_u16_le(binary, 0);
    append_u16_le(binary, 1);
    append_u16_le(binary, 2);
    while ((binary.size() & 3U) != 0) binary.push_back(std::byte{0});
    std::string json = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":42}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    while ((json.size() & 3U) != 0) json.push_back(' ');
    std::vector<std::byte> glb;
    append_u32_le(glb, 0x46546C67U);
    append_u32_le(glb, 2U);
    append_u32_le(glb, static_cast<std::uint32_t>(12U + 8U + json.size() + 8U + binary.size()));
    append_u32_le(glb, static_cast<std::uint32_t>(json.size()));
    append_u32_le(glb, 0x4E4F534AU);
    for (char c : json) glb.push_back(static_cast<std::byte>(c));
    append_u32_le(glb, static_cast<std::uint32_t>(binary.size()));
    append_u32_le(glb, 0x004E4942U);
    glb.insert(glb.end(), binary.begin(), binary.end());
    const std::filesystem::path path = temp / "triangle.glb";
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    }
    const dve::ImportedModelResult imported = dve::import_model(path);
    require(imported.success, "minimal GLB must import");
    require(imported.scene.meshes.size() == 1 && imported.scene.meshes[0].triangles.size() == 1,
            "GLB geometry mismatch");
}

void test_settings_sidecar(const std::filesystem::path& temp) {
    const std::filesystem::path path = temp / "asset.dve-import.json";
    {
        std::ofstream output(path);
        output << R"({
  "defaults": {
    "mode": "shell",
    "voxel_size_meters": 0.25,
    "shellThicknessVoxels": 3,
    "supersample_factor": 2,
    "downsampleCoverageThreshold": 0.5,
    "interior_material": 7,
    "objectId": 1234,
    "maximum_working_voxels": 4096,
    "preserveThinSurface": false
  }
})";
    }
    dve::VoxelizeSettings settings;
    std::string error;
    require(dve::apply_voxelize_settings_json(path, settings, &error), error.c_str());
    require(settings.mode == dve::VoxelizationMode::Shell, "sidecar mode mismatch");
    require(settings.voxelSizeMeters == 0.25F, "sidecar voxel size mismatch");
    require(settings.shellThicknessVoxels == 3, "sidecar shell thickness mismatch");
    require(settings.supersampleFactor == 2, "sidecar supersample mismatch");
    require(settings.downsampleCoverageThreshold == 0.5F, "sidecar coverage mismatch");
    require(settings.interiorMaterial == 7, "sidecar material mismatch");
    require(settings.objectId == 1234, "sidecar object ID mismatch");
    require(settings.maximumWorkingVoxels == 4096, "sidecar working limit mismatch");
    require(!settings.preserveThinSurface, "sidecar thin-surface flag mismatch");
}


void write_textured_plane_gltf(
    const std::filesystem::path& directory,
    std::string_view stem,
    std::string_view pngBase64,
    std::string_view alphaMode) {
    std::vector<std::byte> buffer;
    for (float value : {
        0.1F, 0.1F, 0.5F, 1.9F, 0.1F, 0.5F, 1.9F, 1.9F, 0.5F, 0.1F, 1.9F, 0.5F,
        0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F}) {
        append_float_le(buffer, value);
    }
    for (std::uint16_t index : {0, 1, 2, 0, 2, 3}) append_u16_le(buffer, index);
    const std::filesystem::path bin = directory / (std::string(stem) + ".bin");
    const std::filesystem::path gltf = directory / (std::string(stem) + ".gltf");
    {
        std::ofstream output(bin, std::ios::binary);
        output.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    }
    {
        std::ofstream output(gltf);
        output << "{\n"
            << "\"asset\":{\"version\":\"2.0\"},\n"
            << "\"buffers\":[{\"uri\":\"" << stem << ".bin\",\"byteLength\":92}],\n"
            << "\"bufferViews\":["
            << "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},"
            << "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":32},"
            << "{\"buffer\":0,\"byteOffset\":80,\"byteLength\":12}],\n"
            << "\"accessors\":["
            << "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
            << "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC2\"},"
            << "{\"bufferView\":2,\"componentType\":5123,\"count\":6,\"type\":\"SCALAR\"}],\n"
            << "\"images\":[{\"uri\":\"data:image/png;base64," << pngBase64 << "\"}],\n"
            << "\"samplers\":[{\"magFilter\":9728,\"minFilter\":9728,\"wrapS\":33071,\"wrapT\":33071}],\n"
            << "\"textures\":[{\"source\":0,\"sampler\":0}],\n"
            << "\"materials\":[{\"name\":\"Texture\",\"alphaMode\":\"" << alphaMode
            << "\",\"alphaCutoff\":0.5,\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},\"metallicFactor\":0,\"roughnessFactor\":1}}],\n"
            << "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},\"indices\":2,\"material\":0}]}],\n"
            << "\"nodes\":[{\"name\":\"Panel\",\"mesh\":0,\"extras\":{\"dve\":{\"mode\":\"surface\",\"structural\":false}}}],\n"
            << "\"scenes\":[{\"nodes\":[0]}],\"scene\":0\n} ";
    }
}


void test_glb_embedded_png_texture(const std::filesystem::path& temp) {
    std::vector<std::byte> binary;
    for (float value : {
        0.1F, 0.1F, 0.5F, 1.9F, 0.1F, 0.5F, 1.9F, 1.9F, 0.5F, 0.1F, 1.9F, 0.5F,
        0.0F, 0.0F, 1.0F, 0.0F, 1.0F, 1.0F, 0.0F, 1.0F}) {
        append_float_le(binary, value);
    }
    for (std::uint16_t index : {0, 1, 2, 0, 2, 3}) append_u16_le(binary, index);
    require(binary.size() == 92, "embedded PNG GLB geometry size mismatch");
    const std::filesystem::path pngPath =
        std::filesystem::path(DVE_TEST_SOURCE_DIR) / "tests" / "assets" / "checker_2x2.png";
    const std::vector<std::byte> png = read_bytes(pngPath);
    const std::uint32_t imageOffset = static_cast<std::uint32_t>(binary.size());
    binary.insert(binary.end(), png.begin(), png.end());
    while ((binary.size() & 3U) != 0U) binary.push_back(std::byte{0});

    std::ostringstream jsonStream;
    jsonStream
        << R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":)" << binary.size()
        << R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":48},{"buffer":0,"byteOffset":48,"byteLength":32},{"buffer":0,"byteOffset":80,"byteLength":12},{"buffer":0,"byteOffset":)"
        << imageOffset << R"(,"byteLength":)" << png.size()
        << R"(}],"accessors":[{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3"},{"bufferView":1,"componentType":5126,"count":4,"type":"VEC2"},{"bufferView":2,"componentType":5123,"count":6,"type":"SCALAR"}],"images":[{"bufferView":3,"mimeType":"image/png"}],"samplers":[{"magFilter":9728,"minFilter":9728,"wrapS":33071,"wrapT":33071}],"textures":[{"source":0,"sampler":0}],"materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicFactor":0,"roughnessFactor":1}}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2,"material":0}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    std::string json = jsonStream.str();
    while ((json.size() & 3U) != 0U) json.push_back(' ');

    std::vector<std::byte> glb;
    append_u32_le(glb, 0x46546C67U);
    append_u32_le(glb, 2U);
    append_u32_le(glb, static_cast<std::uint32_t>(12U + 8U + json.size() + 8U + binary.size()));
    append_u32_le(glb, static_cast<std::uint32_t>(json.size()));
    append_u32_le(glb, 0x4E4F534AU);
    for (char c : json) glb.push_back(static_cast<std::byte>(c));
    append_u32_le(glb, static_cast<std::uint32_t>(binary.size()));
    append_u32_le(glb, 0x004E4942U);
    glb.insert(glb.end(), binary.begin(), binary.end());

    const std::filesystem::path path = temp / "embedded_texture.glb";
    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
    }
    const dve::ImportedModelResult imported = dve::import_model(path);
    require(imported.success, "GLB buffer-view PNG texture must import");
    require(imported.scene.images.size() == 1 && imported.scene.images[0].width == 4 &&
                imported.scene.images[0].height == 4,
            "embedded GLB PNG dimensions mismatch");
    dve::VoxelizeSettings settings;
    settings.mode = dve::VoxelizationMode::SurfaceOnly;
    settings.voxelSizeMeters = 1.0F;
    const dve::CookedVoxelAsset asset = dve::voxelize_scene(imported.scene, settings);
    require(asset.object.occupied_voxel_count() == 4, "embedded GLB texture plane should produce four voxels");
    require(asset.stats.decodedImages == 1 && asset.stats.textureSamples >= 4,
            "embedded GLB texture telemetry mismatch");
}

void test_texture_baking_and_palette(const std::filesystem::path& temp) {
    constexpr std::string_view checker =
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEklEQVR4nGP4z8DwHwyBNBgAAEnICff5q7YNAAAAAElFTkSuQmCC";
    write_textured_plane_gltf(temp, "checker", checker, "OPAQUE");
    const dve::ImportedModelResult imported = dve::import_model(temp / "checker.gltf");
    require(imported.success, "textured glTF must import");
    require(imported.scene.images.size() == 1, "PNG image count mismatch");
    require(imported.scene.images[0].width == 2 && imported.scene.images[0].height == 2,
            "PNG image dimensions mismatch");
    dve::VoxelizeSettings settings;
    settings.mode = dve::VoxelizationMode::SurfaceOnly;
    settings.voxelSizeMeters = 1.0F;
    settings.maximumPaletteMaterials = 256;
    const dve::CookedVoxelAsset full = dve::voxelize_scene(imported.scene, settings);
    require(full.object.occupied_voxel_count() == 4, "opaque checker plane should produce four voxels");
    require(full.materials.size() == 5, "checker should preserve four sampled colors plus air");
    require(full.stats.textureSamples >= 4, "texture sampling telemetry was not updated");
    require(full.stats.paletteMaterials == full.materials.size(), "palette telemetry mismatch");

    settings.maximumPaletteMaterials = 3;
    const dve::CookedVoxelAsset reducedA = dve::voxelize_scene(imported.scene, settings);
    const dve::CookedVoxelAsset reducedB = dve::voxelize_scene(imported.scene, settings);
    require(reducedA.materials.size() <= 3, "palette limit was not enforced");
    require(reducedA.object.state_hash() == reducedB.object.state_hash(), "palette reduction must be deterministic");
    const std::filesystem::path a = temp / "palette_a.dvox";
    const std::filesystem::path b = temp / "palette_b.dvox";
    std::string error;
    require(dve::write_dvox(a, reducedA, {}, &error), error.c_str());
    require(dve::write_dvox(b, reducedB, {}, &error), error.c_str());
    require(read_bytes(a) == read_bytes(b), "quantized textured cooks must be byte identical");
}

void test_alpha_mask_baking(const std::filesystem::path& temp) {
    constexpr std::string_view alpha =
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEUlEQVR4nGP4DwEMDCACxAIAgJEN837B/94AAAAASUVORK5CYII=";
    write_textured_plane_gltf(temp, "alpha_grate", alpha, "MASK");
    const dve::ImportedModelResult imported = dve::import_model(temp / "alpha_grate.gltf");
    require(imported.success, "alpha-masked glTF must import");
    dve::VoxelizeSettings settings;
    settings.mode = dve::VoxelizationMode::SurfaceOnly;
    settings.voxelSizeMeters = 1.0F;
    const dve::CookedVoxelAsset asset = dve::voxelize_scene(imported.scene, settings);
    require(asset.object.occupied_voxel_count() == 2, "alpha mask should retain two of four grate cells");
    require(asset.stats.alphaRejectedSamples != 0, "alpha rejection telemetry was not updated");
}


void test_jpeg_texture_import(const std::filesystem::path& temp) {
    constexpr std::string_view placeholderPng =
        "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAYAAABytg0kAAAAEklEQVR4nGP4z8DwHwyBNBgAAEnICff5q7YNAAAAAElFTkSuQmCC";
    write_textured_plane_gltf(temp, "jpeg_checker", placeholderPng, "OPAQUE");

    const std::filesystem::path sourceImage =
        std::filesystem::path(DVE_TEST_SOURCE_DIR) / "tests" / "assets" / "checker_2x2.jpg";
    const std::filesystem::path copiedImage = temp / "checker_2x2.jpg";
    std::error_code copyError;
    std::filesystem::copy_file(sourceImage, copiedImage,
                               std::filesystem::copy_options::overwrite_existing, copyError);
    require(!copyError, "failed to copy packaged JPEG fixture");

    const std::filesystem::path path = temp / "jpeg_checker.gltf";
    const std::vector<std::byte> gltfBytes = read_bytes(path);
    std::string text(reinterpret_cast<const char*>(gltfBytes.data()), gltfBytes.size());
    const std::string dataUriPrefix = "data:image/png;base64,";
    const std::size_t uriPosition = text.find(dataUriPrefix);
    require(uriPosition != std::string::npos, "JPEG fixture URI replacement failed");
    const std::size_t valueEnd = text.find('"', uriPosition);
    require(valueEnd != std::string::npos, "JPEG fixture URI terminator was not found");
    text.replace(uriPosition, valueEnd - uriPosition, "checker_2x2.jpg");


    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        require(static_cast<bool>(output), "failed to rewrite JPEG glTF fixture");
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    const dve::ImportedModelResult imported = dve::import_model(path);
    require(imported.success, "JPEG-textured glTF must import");
    require(imported.scene.images.size() == 1 &&
                imported.scene.images[0].width == 2 && imported.scene.images[0].height == 2,
            "JPEG decode dimensions mismatch");
    require(imported.scene.images[0].mimeType == "image/jpeg", "JPEG MIME classification mismatch");

    dve::VoxelizeSettings settings;
    settings.mode = dve::VoxelizationMode::SurfaceOnly;
    settings.voxelSizeMeters = 1.0F;
    const dve::CookedVoxelAsset asset = dve::voxelize_scene(imported.scene, settings);
    require(asset.object.occupied_voxel_count() == 4, "JPEG textured plane should produce four voxels");
    require(asset.stats.textureSamples >= 4, "JPEG texture sampling telemetry was not updated");
}

void test_multi_object_cooking(const std::filesystem::path& temp) {
    dve::ImportedScene scene = make_cube_scene();
    scene.name = "House";
    scene.nodes[0].name = "Foundation";
    scene.nodes[0].extrasJson = R"({"dve":{"anchored":true,"mode":"solid"}})";
    dve::ImportedNode child;
    child.name = "Furniture";
    child.mesh = 0;
    child.parent = 0;
    child.extrasJson = R"({"dve":{"structural":false,"generateCollision":true}})";
    child.localTransform = dve::Matrix4::identity();
    child.localTransform.values[0] = 0.5F;
    child.localTransform.values[5] = 0.5F;
    child.localTransform.values[10] = 0.5F;
    child.localTransform.values[12] = 3.0F;
    child.worldTransform = child.localTransform;
    scene.nodes[0].children.push_back(1);
    scene.nodes.push_back(child);

    dve::SceneCookSettings settings;
    settings.defaults.voxelSizeMeters = 1.0F;
    settings.defaults.mode = dve::VoxelizationMode::Solid;
    const dve::CookedVoxelScene cooked = dve::voxelize_scene_objects(scene, settings);
    require(cooked.success, "multi-object cooking failed");
    require(cooked.objects.size() == 2, "multi-object cooker should preserve two mesh nodes");
    require(cooked.objects[0].anchored, "node extras anchoring was not applied");
    require(!cooked.objects[1].structural, "node extras structural flag was not applied");
    require(cooked.objects[1].parentObject.has_value() && *cooked.objects[1].parentObject == 0,
            "cooked parent hierarchy mismatch");
    require(cooked.objects[0].asset.object.id() != cooked.objects[1].asset.object.id(),
            "stable object IDs must be distinct");
    require(cooked.objects[1].worldTransform.values[0] == 1.0F &&
                cooked.objects[1].worldTransform.values[5] == 1.0F &&
                cooked.objects[1].worldTransform.values[10] == 1.0F,
            "bakeable node scale must be removed from the published rigid transform");
    require(cooked.objects[1].asset.object.occupied_voxel_count() <
                cooked.objects[0].asset.object.occupied_voxel_count(),
            "baked node scale must change the cooked voxel geometry");

    const std::filesystem::path sidecar = temp / "house_scene_settings.json";
    {
        std::ofstream output(sidecar);
        output << R"({"defaults":{"voxelSizeMeters":1.0,"maximumPaletteMaterials":16},"splitByNode":true,"nodes":{"Furniture":{"ignore":true}}})";
    }
    dve::SceneCookSettings sidecarSettings;
    std::string error;
    require(dve::apply_scene_cook_settings_json(sidecar, sidecarSettings, &error), error.c_str());
    const dve::CookedVoxelScene filtered = dve::voxelize_scene_objects(scene, sidecarSettings);
    require(filtered.success && filtered.objects.size() == 1, "sidecar node ignore rule failed");

    const std::filesystem::path manifest = temp / "house.dvoxscene.json";
    require(dve::write_dvox_scene_package(manifest, cooked, &error), error.c_str());
    require(std::filesystem::exists(manifest), "scene manifest was not written");
    const std::vector<std::byte> manifestBytes = read_bytes(manifest);
    const std::string manifestText(reinterpret_cast<const char*>(manifestBytes.data()), manifestBytes.size());
    require(manifestText.find("Foundation") != std::string::npos && manifestText.find("Furniture") != std::string::npos,
            "scene manifest omitted object records");
    require(manifestText.find("house_Foundation_") != std::string::npos &&
                manifestText.find("house.dvoxscene_Foundation_") == std::string::npos,
            "scene object filenames must use the package stem without the manifest suffix");
    const std::vector<std::byte> firstManifest = read_bytes(manifest);
    require(dve::write_dvox_scene_package(manifest, cooked, &error), error.c_str());
    require(read_bytes(manifest) == firstManifest, "scene package manifest must be byte deterministic");

    constexpr std::uint64_t largeId = 18446744073709550000ULL;
    dve::CookedVoxelScene largeIdScene;
    largeIdScene.name = "LargeIdScene";
    largeIdScene.success = true;
    largeIdScene.objects.emplace_back(largeId);
    dve::CookedVoxelObject& large = largeIdScene.objects.back();
    large.name = "Large";
    large.nodePath = "/Large";
    large.worldTransform = dve::Matrix4::identity();
    large.generateCollision = false;
    large.asset.voxelSizeMeters = 1.0F;
    large.asset.materials.resize(2);
    large.asset.object.set_voxel({0, 0, 0}, 1);
    const std::filesystem::path largeManifest = temp / "large_id.dvoxscene.json";
    require(dve::write_dvox_scene_package(largeManifest, largeIdScene, &error), error.c_str());
    const std::vector<std::byte> largeBytes = read_bytes(largeManifest);
    const std::string largeText(reinterpret_cast<const char*>(largeBytes.data()), largeBytes.size());
    require(largeText.find("\"id\":\"18446744073709550000\"") != std::string::npos,
            "uint64 IDs above JSON's exact numeric range must be serialized as decimal strings");
}


void test_non_rigid_collision_transform_rejected() {
    dve::ImportedScene scene = make_cube_scene();
    scene.name = "ShearedScene";
    scene.nodes[0].name = "ShearedBody";
    scene.nodes[0].worldTransform.values[4] = 0.25F;
    dve::SceneCookSettings settings;
    settings.splitByNode = true;
    settings.defaults.voxelSizeMeters = 1.0F;
    const dve::CookedVoxelScene cooked = dve::voxelize_scene_objects(scene, settings);
    require(!cooked.success, "collision-enabled sheared node must be rejected");
    bool found = false;
    for (const dve::ImportDiagnostic& diagnostic : cooked.diagnostics) {
        found = found || diagnostic.code == "NON_RIGID_NODE_TRANSFORM";
    }
    require(found, "sheared node rejection diagnostic was not emitted");
}

} // namespace

int main() {
    try {
        const std::filesystem::path temp = std::filesystem::temp_directory_path() / "dve_asset_cooker_tests";
        std::filesystem::remove_all(temp);
        std::filesystem::create_directories(temp);
        test_cube_voxelization_and_dvox_roundtrip(temp);
        test_dvox_all_encodings(temp);
        test_dvox_full_content_hash_and_size_limit(temp);
        test_settings_sidecar(temp);
        test_surface_and_shell_modes();
        test_gltf_import(temp);
        test_glb_import(temp);
        test_glb_embedded_png_texture(temp);
        test_texture_baking_and_palette(temp);
        test_alpha_mask_baking(temp);
        test_jpeg_texture_import(temp);
        test_multi_object_cooking(temp);
        test_non_rigid_collision_transform_rejected();
        std::filesystem::remove_all(temp);
        std::cout << "dve_asset_cooker_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_asset_cooker_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
