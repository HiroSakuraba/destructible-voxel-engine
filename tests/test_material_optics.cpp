// Material optics (transmission, index of refraction, attenuation): glass, water and gems.
//
// Defaults change nothing (no optics block, old versions and hashes); materials with optics
// round-trip through DVOX v1.4, DMESH v1.4 and the editor library v4; out-of-range values are
// refused; glTF KHR_materials_transmission / _ior / _volume import; layers blend the optics.
#include "dve/voxel_normal_map.hpp"
#include "dve/voxel_textures.hpp"
#include "dve/asset_cooker.hpp"
#include "dve/dvox.hpp"
#include "dve/editor_materials.hpp"
#include "dve/gpu_material_buffer.hpp"
#include "dve/master_material.hpp"
#include "dve/polygon_asset.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
using namespace dve;
using dve::editor::EditorMaterialLibrary;
namespace fs = std::filesystem;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}

fs::path temp_dir() {
    const auto dir = fs::temp_directory_path() /
                     ("dve_optics_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    return dir;
}
std::uint16_t file_minor(const fs::path& path, std::size_t offset) {
    std::ifstream in(path, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(offset));
    unsigned char b[2]{};
    in.read(reinterpret_cast<char*>(b), 2);
    return static_cast<std::uint16_t>(b[0] | (b[1] << 8U));
}

VoxelMaterialDefinition glass() {
    VoxelMaterialDefinition m;
    m.name = "Glass";
    m.transmission = 0.95F;
    m.indexOfRefraction = 1.52F;
    m.attenuationColor = {0.7F, 0.9F, 0.8F};
    m.attenuationDistanceMeters = 0.5F;
    return m;
}
bool same_optics(const VoxelMaterialDefinition& a, const VoxelMaterialDefinition& b) {
    return a.transmission == b.transmission && a.indexOfRefraction == b.indexOfRefraction &&
           a.attenuationColor.x == b.attenuationColor.x && a.attenuationColor.y == b.attenuationColor.y &&
           a.attenuationColor.z == b.attenuationColor.z && a.attenuationDistanceMeters == b.attenuationDistanceMeters;
}

void test_defaults_and_validation() {
    VoxelMaterialDefinition m;
    check(!material_has_optics(m) && !material_is_refractive(m) && material_optics_valid(m), "defaults: no optics");
    m = glass();
    check(material_has_optics(m) && material_is_refractive(m) && material_optics_valid(m), "glass");
    for (int which = 0; which < 5; ++which) {
        VoxelMaterialDefinition b = glass();
        if (which == 0) b.indexOfRefraction = 0.5F;
        if (which == 1) b.transmission = 1.5F;
        if (which == 2) b.attenuationColor.y = -0.1F;
        if (which == 3) b.attenuationDistanceMeters = -1.0F;
        if (which == 4) b.indexOfRefraction = 4.0F;
        check(!material_optics_valid(b), "out of range refused");
    }
}

void test_dvox(const fs::path& dir) {
    CookedVoxelAsset plain(11);
    plain.materials.resize(2);
    plain.materials[1].name = "Stone";
    plain.object.set_voxel({0, 0, 0}, 1);
    std::string error;
    check(write_dvox(dir / "plain.dvox", plain, {}, &error), "plain dvox: " + error);
    check(file_minor(dir / "plain.dvox", 6) == 2, "materials without optics keep DVOX v1.2");
    CookedVoxelAsset lens(12);
    lens.materials = plain.materials;
    lens.materials[1] = glass();
    lens.object.set_voxel({0, 0, 0}, 1);
    check(write_dvox(dir / "glass.dvox", lens, {}, &error), "glass dvox: " + error);
    check(file_minor(dir / "glass.dvox", 6) == 4, "optics need DVOX v1.4");
    const auto back = read_dvox(dir / "glass.dvox");
    check(back.success && back.asset.materials.size() == 2 && same_optics(back.asset.materials[1], lens.materials[1]),
          "DVOX optics round-trip: " + back.error);
    const auto plainBack = read_dvox(dir / "plain.dvox");
    check(plainBack.success && !material_has_optics(plainBack.asset.materials[1]), "plain stays plain");
    lens.materials[1].indexOfRefraction = 0.2F;
    check(!write_dvox(dir / "bad.dvox", lens, {}, &error), "invalid optics are not written");
}

CookedPolygonAsset triangle(const VoxelMaterialDefinition& material) {
    CookedPolygonAsset a;
    a.objectId = 21;
    a.materials.push_back(material);
    a.materialBindings.resize(1);
    a.vertices = {{{0, 0, 0}, {0, 0, 1}, {1, 0, 0, 1}, {0, 0}, {1, 1, 1, 1}, {0, 0}},
                  {{1, 0, 0}, {0, 0, 1}, {1, 0, 0, 1}, {1, 0}, {1, 1, 1, 1}, {1, 0}},
                  {{0, 1, 0}, {0, 0, 1}, {1, 0, 0, 1}, {0, 1}, {1, 1, 1, 1}, {0, 1}}};
    a.indices = {0, 1, 2};
    a.submeshes.push_back({"tri", 0, 3, 0});
    a.bounds = {{0, 0, 0}, {1, 1, 0}};
    return a;
}

void test_dmesh(const fs::path& dir) {
    VoxelMaterialDefinition plainMaterial;
    plainMaterial.name = "Plain";
    const CookedPolygonAsset plain = triangle(plainMaterial);
    std::string error;
    check(write_dmesh(dir / "plain.dmesh", plain, &error), "plain dmesh: " + error);
    check(file_minor(dir / "plain.dmesh", 10) == 3, "materials without optics keep DMESH v1.3");
    const std::uint64_t plainHash = polygon_asset_content_hash(plain);
    CookedPolygonAsset clear = plain;
    clear.materials[0].indexOfRefraction = 1.5F;   // the default: still no optics
    check(polygon_asset_content_hash(clear) == plainHash, "default optics leave the content hash alone");
    const CookedPolygonAsset lens = triangle(glass());
    check(polygon_asset_content_hash(lens) != polygon_asset_content_hash(triangle(VoxelMaterialDefinition{glass().name})),
          "optics change the content hash");
    check(write_dmesh(dir / "glass.dmesh", lens, &error), "glass dmesh: " + error);
    check(file_minor(dir / "glass.dmesh", 10) == 4, "optics need DMESH v1.4");
    const auto back = read_dmesh(dir / "glass.dmesh");
    check(back && same_optics(back.asset.materials[0], lens.materials[0]), "DMESH optics round-trip: " + back.error);
    CookedPolygonAsset bad = lens;
    bad.materials[0].transmission = 2.0F;
    check(!validate_polygon_asset(bad), "polygon validation refuses bad optics");
}

void test_editor_library(const fs::path& dir) {
    EditorMaterialLibrary library;
    const auto* clearGlass=library.find(5);
    check(clearGlass&&clearGlass->definition.transmission==1.0F&&clearGlass->definition.baseColor.w==1.0F&&
          clearGlass->definition.indexOfRefraction==1.5F&&clearGlass->definition.roughness==0.0F,
          "default glass is a covered dielectric, not alpha paint");
    VoxelMaterialDefinition water = glass();
    water.name = "Water";
    water.indexOfRefraction = 1.33F;
    const MaterialId id = library.next_available_id();
    std::string error;
    check(library.upsert({id, water, false}, &error), "library add: " + error);
    check(library.save(dir / "materials.txt", &error), "library save: " + error);
    const auto loaded = EditorMaterialLibrary::load(dir / "materials.txt", &error);
    check(loaded.has_value(), "library load: " + error);
    if (loaded) {
        const auto* entry = loaded->find(id);
        check(entry && same_optics(entry->definition, water), "library optics round-trip");
    }
    water.indexOfRefraction = 5.0F;
    check(!library.upsert({id, water, false}, &error), "library refuses bad optics");

    // Normal maps: version 5 only when a material has one; texels and parameters round-trip.
    {
        std::ifstream in(dir / "materials.txt");
        std::string magic;
        unsigned version{};
        in >> magic >> version;
        check(version == 4U, "a library without normal maps stays version 4");
    }
    VoxelMaterialDefinition brick;
    brick.name = "Brick";
    brick.normalMap = make_voxel_normal_map("brick normal", 2U, 2U, {10, 20, 250, 255, 128, 128, 255, 255, 200, 90, 230, 255, 0, 255, 128, 7});
    brick.normalMapTileMeters = 0.25F;
    brick.normalMapStrength = 2.0F;
    brick.normalMapSharpness = 16.0F;
    const MaterialId brickId = library.next_available_id();
    check(library.upsert({brickId, brick, false}, &error), "library add normal-mapped material: " + error);
    check(library.save(dir / "materials5.txt", &error), "library save v5: " + error);
    const auto five = EditorMaterialLibrary::load(dir / "materials5.txt", &error);
    check(five.has_value(), "library v5 load: " + error);
    if (five) {
        const auto* entry = five->find(brickId);
        check(entry && entry->definition.normalMap && entry->definition.normalMap->rgba8 == brick.normalMap->rgba8 &&
                  entry->definition.normalMap->name == "brick normal" && entry->definition.normalMapTileMeters == 0.25F &&
                  entry->definition.normalMapStrength == 2.0F && entry->definition.normalMapSharpness == 16.0F,
              "library normal map round-trip");
        const auto* plainEntry = five->find(id);
        check(plainEntry && !plainEntry->definition.normalMap, "materials without maps stay without");
    }
    brick.normalMapStrength = 9.0F;
    check(!library.upsert({brickId, brick, false}, &error), "library refuses a strength above 4");

    // Texture sets: version 10, maps with their roles and the projection round-trip.
    VoxelMaterialDefinition stone;
    stone.name = "Stone";
    stone.baseColorMap = make_voxel_image("stone albedo", 2U, 2U, {200, 100, 50, 255, 10, 20, 30, 255, 1, 2, 3, 4, 90, 80, 70, 60}, VoxelImageRole::Color);
    stone.ormMap = make_voxel_image("stone orm", 1U, 1U, {255, 128, 0, 255}, VoxelImageRole::Data);
    stone.textureTileMeters = 0.5F;
    stone.textureSharpness = 12.0F;
    const MaterialId stoneId = library.next_available_id();
    check(library.upsert({stoneId, stone, false}, &error), "library add textured material: " + error);
    check(library.save(dir / "materials10.txt", &error), "library save v10: " + error);
    {
        std::ifstream in(dir / "materials10.txt");
        std::string magic;
        unsigned version{};
        in >> magic >> version;
        check(version == 10U, "texture sets need library version 10");
    }
    const auto ten = EditorMaterialLibrary::load(dir / "materials10.txt", &error);
    check(ten.has_value(), "library v10 load: " + error);
    if (ten) {
        const auto* entry = ten->find(stoneId);
        check(entry && entry->definition.baseColorMap && entry->definition.ormMap && !entry->definition.heightMap &&
                  entry->definition.baseColorMap->contentHash == stone.baseColorMap->contentHash &&
                  entry->definition.ormMap->contentHash == stone.ormMap->contentHash && entry->definition.textureTileMeters == 0.5F &&
                  entry->definition.textureSharpness == 12.0F,
              "library texture-set round trip");
    }
}

void test_gltf_import(const fs::path& dir) {
    // One triangle, positions in a data URI (3 x float3 = 36 bytes).
    const char* gltf = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
 "meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],
 "materials":[{"name":"Lens","pbrMetallicRoughness":{"baseColorFactor":[1,1,1,1],"metallicFactor":0,"roughnessFactor":0.05},
   "extensions":{"KHR_materials_transmission":{"transmissionFactor":1.0},"KHR_materials_ior":{"ior":1.45},
                 "KHR_materials_volume":{"thicknessFactor":0.02,"attenuationColor":[0.9,0.95,1.0],"attenuationDistance":0.3}}}],
 "extensionsUsed":["KHR_materials_transmission","KHR_materials_ior","KHR_materials_volume"],
 "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],
 "bufferViews":[{"buffer":0,"byteLength":36}],
 "buffers":[{"byteLength":36,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}]})";
    std::ofstream(dir / "lens.gltf") << gltf;
    const auto imported = import_model(dir / "lens.gltf");
    check(imported.success && !imported.scene.materials.empty(), "glTF import");
    if (!imported.success || imported.scene.materials.empty()) {
        for (const auto& d : imported.diagnostics) std::printf("  %s\n", d.message.c_str());
        return;
    }
    const ImportedMaterial& m = imported.scene.materials[0];
    check(m.transmissionFactor == 1.0F && m.ior == 1.45F && m.attenuationColor.y == 0.95F && m.attenuationDistance == 0.3F,
          "KHR_materials_* read");
    const CookedPolygonAsset mesh = cook_polygon_scene(imported.scene);
    check(!mesh.materials.empty() && mesh.materials[0].transmission == 1.0F && mesh.materials[0].indexOfRefraction == 1.45F &&
              mesh.materials[0].attenuationDistanceMeters == 0.3F,
          "cooked polygon material carries the optics");
}

void test_gpu_and_layers() {
    const GpuMaterialRecord record = pack_gpu_material_record(glass());
    check(record.transmission == 0.95F && record.indexOfRefraction == 1.52F && record.attenuationColorR == 0.7F,
          "GPU record");
    // A 50% lerp layer of water over glass: halfway optics.
    std::vector<VoxelMaterialDefinition> materials(3);
    materials[1] = glass();
    materials[2] = glass();
    materials[2].indexOfRefraction = 1.32F;
    materials[2].transmission = 0.75F;
    materials[1].layers.push_back({2, 0.5F, MaterialLayerBlendMode::Lerp, true});
    const auto resolved = resolve_voxel_material_layers(materials);
    check(resolved && std::abs((*resolved)[1].indexOfRefraction - 1.42F) < 1e-5F &&
              std::abs((*resolved)[1].transmission - 0.85F) < 1e-5F,
          "layers blend the optics");
}
}  // namespace

int main() {
    const fs::path dir = temp_dir();
    test_defaults_and_validation();
    test_dvox(dir);
    test_dmesh(dir);
    test_editor_library(dir);
    test_gltf_import(dir);
    test_gpu_and_layers();
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (g_failures == 0) std::printf("material optics tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
