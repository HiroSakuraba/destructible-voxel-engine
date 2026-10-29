// Writes the "GI showcase" project for dve_player: a Cornell-box style room built to make
// ambient occlusion and indirect lighting easy to see and to compare between renderers.
//
//   dve_gi_showcase_generator <output-project-dir>
//
// The room has a red left wall, a green right wall, white back wall/floor/ceiling, a skylight
// that lets a low sun in at an angle (lit patches next to deep shadow), a coved (curved)
// floor-to-wall corner, a tilted tall box, a short box, and a bumpy dome whose surface is
// partly shadowed. Q/E turn the sun around the room, Z/X raise and lower it (see main.lua).
#include "dve/asset_cooker.hpp"
#include "dve/dvox.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {

dve::VoxelMaterialDefinition material(std::string name, dve::Float4 color, float roughness, float metallic = 0.0F) {
    dve::VoxelMaterialDefinition result;
    result.name = std::move(name);
    result.baseColor = color;
    result.roughness = roughness;
    result.metallic = metallic;
    result.densityKilogramsPerCubicMeter = 1500.0F;
    return result;
}

bool write_asset(const std::filesystem::path& path, std::uint64_t objectId,
                 std::vector<dve::VoxelMaterialDefinition> materials,
                 const std::function<void(dve::VoxelObject&)>& fill, float voxelSize = 0.1F) {
    dve::CookedVoxelAsset asset(objectId);
    asset.voxelSizeMeters = voxelSize;
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

void box(dve::VoxelObject& o, int x0, int y0, int z0, int x1, int y1, int z1, dve::MaterialId id) {
    for (int z = z0; z <= z1; ++z)
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) o.set_voxel({x, y, z}, id);
}

bool write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
    return static_cast<bool>(file);
}

std::string matrix(float yawDegrees, float x, float y, float z) {
    const float a = yawDegrees * 3.14159265F / 180.0F;
    const float c = std::cos(a), s = std::sin(a);
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer), "[%.6f,0,%.6f,0,0,1,0,0,%.6f,0,%.6f,0,%.4f,%.4f,%.4f,1]", c, -s, s, c, x, y, z);
    return buffer;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: dve_gi_showcase_generator <output-project-dir>\n";
        return 2;
    }
    const std::filesystem::path root = argv[1];
    const std::filesystem::path scenes = root / "scenes";
    std::filesystem::create_directories(scenes);
    const auto unused = material("unused", {0, 0, 0, 1}, 1.0F);
    bool ok = true;

    // Room: interior 3.2 m cube (x,z in [-16,15] voxels, y in [0,31]); front (+z) open.
    ok = write_asset(scenes / "room.dvox", 3001,
        {unused, material("white", {0.76F, 0.74F, 0.70F, 1}, 0.9F), material("red", {0.65F, 0.06F, 0.05F, 1}, 0.9F),
                 material("green", {0.12F, 0.45F, 0.10F, 1}, 0.9F)},
        [](dve::VoxelObject& o) {
            box(o, -18, -2, -18, 17, -1, 17, 1);                 // floor (top at y = 0)
            box(o, -18, 0, -18, -17, 33, 17, 2);                 // left wall, red
            box(o, 16, 0, -18, 17, 33, 17, 3);                   // right wall, green
            box(o, -16, 0, -18, 15, 33, -17, 1);                 // back wall
            for (int z = -18; z <= 17; ++z)                      // ceiling with a skylight
                for (int x = -18; x <= 17; ++x) {
                    const bool hole = x >= 2 && x <= 15 && z >= -4 && z <= 9;
                    if (!hole) { o.set_voxel({x, 32, z}, 1); o.set_voxel({x, 33, z}, 1); }
                }
            // Curved cove between the back wall and the floor (radius 0.6 m).
            for (int z = -16; z <= -10; ++z)
                for (int y = 0; y <= 6; ++y) {
                    const float dz = static_cast<float>(z + 10), dy = static_cast<float>(y - 6);
                    if (dz * dz + dy * dy > 36.0F)
                        for (int x = -16; x <= 15; ++x) o.set_voxel({x, y, z}, 1);
                }
        }) && ok;
    ok = write_asset(scenes / "tall_box.dvox", 3002, {unused, material("white", {0.76F, 0.74F, 0.70F, 1}, 0.85F)},
                     [](dve::VoxelObject& o) { box(o, -5, 0, -5, 4, 17, 4, 1); }) && ok;
    ok = write_asset(scenes / "short_box.dvox", 3003, {unused, material("white", {0.76F, 0.74F, 0.70F, 1}, 0.85F)},
                     [](dve::VoxelObject& o) { box(o, -5, 0, -5, 4, 8, 4, 1); }) && ok;
    // Bumpy dome: a half sphere whose radius ripples with angle, so its surface has ridges and
    // dimples that the light skims across and that trap ambient occlusion.
    ok = write_asset(scenes / "dome.dvox", 3004,
        {unused, material("clay", {0.70F, 0.45F, 0.28F, 1}, 0.55F), material("glaze", {0.20F, 0.35F, 0.55F, 1}, 0.25F)},
        [](dve::VoxelObject& o) {
            for (int z = -14; z <= 14; ++z)
                for (int y = 0; y <= 14; ++y)
                    for (int x = -14; x <= 14; ++x) {
                        const float fx = static_cast<float>(x) + 0.5F, fy = static_cast<float>(y) + 0.5F,
                                    fz = static_cast<float>(z) + 0.5F;
                        const float r = std::sqrt(fx * fx + fy * fy + fz * fz);
                        const float theta = std::atan2(fz, fx);
                        const float phi = std::acos(std::min(1.0F, fy / std::max(r, 1.0e-3F)));
                        const float radius = 10.0F + 1.6F * std::sin(7.0F * theta) * std::sin(6.0F * phi);
                        if (r <= radius) o.set_voxel({x, y, z}, ((x + z) & 4) ? 2 : 1);
                    }
        }, 0.05F) && ok;

    const std::string objects =
        std::string("    {\"index\":0,\"id\":3001,\"name\":\"Room\",\"nodePath\":\"/Room\",\"file\":\"room.dvox\",\"parent\":null,\"anchored\":true,\"structural\":true,\"generateCollision\":true,\"worldMatrix\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]},\n") +
        "    {\"index\":1,\"id\":3002,\"name\":\"TallBox\",\"nodePath\":\"/TallBox\",\"file\":\"tall_box.dvox\",\"parent\":null,\"anchored\":true,\"structural\":false,\"generateCollision\":true,\"worldMatrix\":" + matrix(18.0F, -0.6F, 0.0F, -0.75F) + "},\n" +
        "    {\"index\":2,\"id\":3003,\"name\":\"ShortBox\",\"nodePath\":\"/ShortBox\",\"file\":\"short_box.dvox\",\"parent\":null,\"anchored\":true,\"structural\":false,\"generateCollision\":true,\"worldMatrix\":" + matrix(-16.0F, 0.75F, 0.0F, 0.1F) + "},\n" +
        "    {\"index\":3,\"id\":3004,\"name\":\"Dome\",\"nodePath\":\"/Dome\",\"file\":\"dome.dvox\",\"parent\":null,\"anchored\":true,\"structural\":false,\"generateCollision\":true,\"worldMatrix\":" + matrix(0.0F, -0.65F, 0.0F, 0.85F) + "}\n";
    ok = write_text(scenes / "main.dvoxscene.json",
                    "{\n  \"format\": \"DVOXSCENE\",\n  \"version\": 1,\n  \"name\": \"GiShowcase\",\n  \"objects\": [\n" + objects + "  ]\n}\n") && ok;
    ok = write_text(root / "game.dvegame",
        "DVE_GAME 1\n# GI showcase: a Cornell-box style room for comparing ambient occlusion and indirect light.\n"
        "name=GI Showcase\nversion=0.1.0\nentryScene=scenes/main.dvoxscene.json\nstartupScript=scripts/main.lua\n"
        "settings=game_settings.txt\ncamera=0,1.5,6.2 -> 0,1.45,0\n"
        "bind.sun_left=key:q\nbind.sun_right=key:e\nbind.sun_up=key:x\nbind.sun_down=key:z\nbind.sun_reset=key:r\nbind.ao_view=key:t\n") && ok;
    ok = write_text(root / "game_settings.txt",
        "DVE_GAME_SETTINGS=2\nwidth=1280\nheight=720\nframeRateLimit=60\nfieldOfViewDegrees=60\nrenderScale=1\n"
        "masterVolume=0.8\ndisplayMode=windowed\n") && ok;
    std::filesystem::create_directories(root / "scripts");
    ok = write_text(root / "scripts" / "main.lua",
        "-- GI showcase: Q/E turn the sun around the room, Z/X lower/raise it, R resets,\n"
        "-- T toggles an ambient-only view (no sun, white sky) where the image IS the occlusion.\n"
        "local azimuth, elevation = 0.55, 0.95   -- radians\n"
        "local function apply()\n"
        "  local c = math.cos(elevation)\n"
        "  world.set_environment_vector(\"SunDirection\", math.cos(azimuth) * c, math.sin(elevation), math.sin(azimuth) * c)\n"
        "end\n"
        "world.set_environment_scalar(\"SunIntensity\", 3.0)\n"
        "world.set_environment_scalar(\"AmbientOcclusionSamples\", 16)\n"
        "world.set_environment_scalar(\"AmbientOcclusionMaxDistanceMeters\", 0.9)\n"
        "local aoView, held = false, false\n"
        "local function look()\n"
        "  if aoView then\n"
        "    world.set_environment_scalar(\"SunIntensity\", 0.0)\n"
        "    world.set_environment_vector(\"SkyColor\", 1.0, 1.0, 1.0)\n"
        "    world.set_environment_vector(\"GroundColor\", 1.0, 1.0, 1.0)\n"
        "  else\n"
        "    world.set_environment_scalar(\"SunIntensity\", 3.0)\n"
        "    world.set_environment_vector(\"SkyColor\", 0.50, 0.62, 0.90)\n"
        "    world.set_environment_vector(\"GroundColor\", 0.30, 0.28, 0.25)\n"
        "  end\n"
        "end\n"
        "look()\n"
        "apply()\n"
        "world.on_tick(function(dt)\n"
        "  local turn = 1.2 * dt\n"
        "  if world.is_action_pressed(\"sun_left\") then azimuth = azimuth - turn end\n"
        "  if world.is_action_pressed(\"sun_right\") then azimuth = azimuth + turn end\n"
        "  if world.is_action_pressed(\"sun_up\") then elevation = math.min(1.5, elevation + 0.6 * dt) end\n"
        "  if world.is_action_pressed(\"sun_down\") then elevation = math.max(0.15, elevation - 0.6 * dt) end\n"
        "  local t = world.is_action_pressed(\"ao_view\")\n"
        "  if t and not held then aoView = not aoView; look() end\n"
        "  held = t\n"
        "  if world.is_action_pressed(\"sun_reset\") then azimuth, elevation = 0.55, 0.95 end\n"
        "  apply()\n"
        "end)\n") && ok;
    return ok ? 0 : 1;
}
