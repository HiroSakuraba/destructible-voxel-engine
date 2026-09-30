// Regenerates the voxel assets of the dve_player sample game (tests/data/player_sample).
//
//   dve_player_sample_generator <output-dir>
//
// Writes scenes/*.dvox next to the checked-in scenes/main.dvoxscene.json. The assets are
// procedural and deterministic, so the test `dve_player_sample_assets_up_to_date` can compare
// a fresh run against the checked-in files byte for byte.
#include "dve/asset_cooker.hpp"
#include "dve/dvox.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {

dve::VoxelMaterialDefinition material(std::string name, dve::Float4 color, float roughness,
                                      float density, dve::Float3 emissive = {}) {
    dve::VoxelMaterialDefinition result;
    result.name = std::move(name);
    result.baseColor = color;
    result.roughness = roughness;
    result.emissive = emissive;
    result.densityKilogramsPerCubicMeter = density;
    return result;
}

struct Box {
    int x0, y0, z0, x1, y1, z1; // inclusive
};

bool write_asset(const std::filesystem::path& path, std::uint64_t objectId,
                 std::vector<dve::VoxelMaterialDefinition> materials,
                 const std::function<void(dve::VoxelObject&)>& fill) {
    dve::CookedVoxelAsset asset(objectId);
    asset.voxelSizeMeters = 0.1F;
    asset.materials = std::move(materials);
    fill(asset.object);
    std::string error;
    if (!dve::write_dvox(path, asset, {}, &error)) {
        std::cerr << "write " << path.generic_string() << " failed: " << error << '\n';
        return false;
    }
    std::cout << path.filename().generic_string() << ": " << asset.object.occupied_voxel_count() << " voxels\n";
    return true;
}

void fill_box(dve::VoxelObject& object, Box box, const std::function<dve::MaterialId(int, int, int)>& pick) {
    for (int z = box.z0; z <= box.z1; ++z)
        for (int y = box.y0; y <= box.y1; ++y)
            for (int x = box.x0; x <= box.x1; ++x) {
                const dve::MaterialId id = pick(x, y, z);
                if (id != 0U) object.set_voxel({x, y, z}, id);
            }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: dve_player_sample_generator <output-dir>\n";
        return 2;
    }
    const std::filesystem::path out = std::filesystem::path(argv[1]) / "scenes";
    std::error_code ec;
    std::filesystem::create_directories(out, ec);
    const auto unused = material("unused", {0, 0, 0, 1}, 1.0F, 1000.0F);
    bool ok = true;

    // Ground: 4.8 m x 0.2 m x 4.8 m checkerboard slab centered on the origin, top at y = 0.
    ok = write_asset(out / "ground.dvox", 2001,
        {unused, material("grass", {0.20F, 0.42F, 0.16F, 1}, 0.95F, 1500.0F),
                 material("moss", {0.13F, 0.30F, 0.12F, 1}, 0.95F, 1500.0F)},
        [](dve::VoxelObject& o) {
            fill_box(o, {-24, -2, -24, 23, -1, 23}, [](int x, int, int z) {
                return static_cast<dve::MaterialId>((((x + 24) / 4 + (z + 24) / 4) % 2) + 1);
            });
        }) && ok;

    // Tower: 0.8 x 2.4 x 0.8 m brick column with a window slot and a light cap.
    ok = write_asset(out / "tower.dvox", 2002,
        {unused, material("brick", {0.55F, 0.20F, 0.09F, 1}, 0.85F, 1900.0F),
                 material("mortar", {0.62F, 0.58F, 0.52F, 1}, 0.9F, 1800.0F),
                 material("lamp", {1.0F, 0.85F, 0.45F, 1}, 0.4F, 1200.0F, {2.5F, 1.9F, 0.8F})},
        [](dve::VoxelObject& o) {
            fill_box(o, {-4, 0, -4, 3, 23, 3}, [](int x, int y, int z) -> dve::MaterialId {
                if (y >= 14 && y <= 17 && (x == -1 || x == 0) && z == 3) return 0; // window
                if (y >= 22) return 3;
                return (y % 4 == 3) ? 2 : 1;
            });
        }) && ok;

    // Crate: 0.6 m wooden box with darker edges.
    ok = write_asset(out / "crate.dvox", 2003,
        {unused, material("plank", {0.62F, 0.44F, 0.22F, 1}, 0.8F, 600.0F),
                 material("frame", {0.35F, 0.22F, 0.10F, 1}, 0.8F, 700.0F)},
        [](dve::VoxelObject& o) {
            fill_box(o, {-3, 0, -3, 2, 5, 2}, [](int x, int y, int z) -> dve::MaterialId {
                const int edges = (x == -3 || x == 2) + (y == 0 || y == 5) + (z == -3 || z == 2);
                return edges >= 2 ? 2 : 1;
            });
        }) && ok;

    // Spinner: visual-only (generateCollision=false) floating cross, turned by the Lua script.
    ok = write_asset(out / "spinner.dvox", 2004,
        {unused, material("cyan", {0.10F, 0.65F, 0.85F, 1}, 0.3F, 500.0F),
                 material("gold", {0.95F, 0.72F, 0.20F, 1}, 0.35F, 500.0F)},
        [](dve::VoxelObject& o) {
            fill_box(o, {-4, -1, -1, 3, 0, 0}, [](int, int, int) -> dve::MaterialId { return 1; });
            fill_box(o, {-1, -4, -1, 0, 3, 0}, [](int, int, int) -> dve::MaterialId { return 2; });
        }) && ok;

    return ok ? 0 : 1;
}
