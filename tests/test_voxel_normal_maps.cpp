// Normal maps on voxel materials (dve/voxel_normal_map.hpp):
//   - a flat map, or strength 0, leaves the image exactly as without a map;
//   - a constant tilted map lights a floor exactly as a surface with the tilted normal would;
//   - the projection repeats every tile and moves with the instance (a translated object and
//     camera draw the same image);
//   - a bumpy map varies the shading and the normal pass;
//   - DVOX writes v1.5 only with maps and round-trips them; files without maps are unchanged;
//   - PNG import (no colour conversion) and size limits.
#include "dve/asset_cooker.hpp"
#include "dve/dvox.hpp"
#include "dve/render/polygon_renderer.hpp"
#include "dve/render/voxel_reference_renderer.hpp"
#include "dve/voxel_normal_map.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
using namespace dve;
using namespace dve::render;
namespace fs = std::filesystem;

int g_failures = 0;
void check(bool condition, const std::string& message) {
    if (!condition) { std::printf("FAIL: %s\n", message.c_str()); ++g_failures; }
}

std::shared_ptr<const VoxelNormalMap> constant_map(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    std::vector<std::uint8_t> texels;
    for (int i = 0; i < 4; ++i) texels.insert(texels.end(), {r, g, b, 255});
    return make_voxel_normal_map("constant", 2U, 2U, texels);
}

std::shared_ptr<const VoxelNormalMap> bumpy_map() {
    // Bumps: a grid of hemispheres, 32 x 32.
    std::vector<std::uint8_t> texels;
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            const float u = std::fmod(static_cast<float>(x) / 8.0F, 1.0F) * 2.0F - 1.0F;
            const float v = std::fmod(static_cast<float>(y) / 8.0F, 1.0F) * 2.0F - 1.0F;
            const float z = std::sqrt(std::max(0.05F, 1.0F - u * u - v * v));
            const Float3 n = normalize(Float3{u * 0.7F, -v * 0.7F, z});
            texels.insert(texels.end(), {static_cast<std::uint8_t>(std::lround((n.x + 1.0F) * 127.5F)),
                                         static_cast<std::uint8_t>(std::lround((n.y + 1.0F) * 127.5F)),
                                         static_cast<std::uint8_t>(std::lround((n.z + 1.0F) * 127.5F)), 255});
        }
    return make_voxel_normal_map("bumps", 32U, 32U, texels);
}

struct Floor {
    VoxelObject object{1U};
    std::array<VoxelMaterialDefinition, 2> materials{};
    explicit Floor(const VoxelMaterialDefinition& m) {
        materials[1] = m;
        for (int x = -30; x < 30; ++x)
            for (int z = -30; z < 30; ++z) object.set_voxel({x, 0, z}, 1);
    }
};

VoxelMaterialDefinition matte() {
    VoxelMaterialDefinition m;
    m.baseColor = {0.6F, 0.6F, 0.6F, 1.0F};
    m.specular = 0.0F;   // Lambert, so expected values are exact
    return m;
}

RenderEnvironment sun_only() {
    RenderEnvironment e;
    e.sunDirection = normalize(Float3{0.5F, 0.8F, 0.2F});
    e.sunIntensity = 2.0F;
    e.sunColor = {1, 1, 1};
    e.shadowMode = ShadowMode::Off;
    e.globalIlluminationMode = GlobalIlluminationMode::Off;
    e.ambientOcclusionSamples = 0;
    return e;
}

PolygonCamera above(Float3 at) {
    PolygonCamera c;
    c.position = {at.x, at.y + 20.0F, at.z + 0.001F};
    c.target = at;
    c.up = {0, 0, -1};
    c.verticalFieldOfViewRadians = 0.8F;
    c.nearPlane = 0.5F;
    c.farPlane = 400.0F;
    return c;
}

PolygonRenderTarget draw(const VoxelMaterialDefinition& m, const RenderEnvironment& e, RigidTransform transform = {},
                         Float3 at = {0.3F, 1.0F, 0.2F}) {
    Floor floor(m);
    const std::vector<VoxelReferenceInstance> instances{{1U, &floor.object, transform, floor.materials, true, {1, 1, 1}}};
    PolygonRenderTarget target;
    target.resize(48, 48);
    target.enable_passes();
    ReferenceVoxelRenderer renderer;
    renderer.threadCount = 2;
    (void)renderer.render(instances, above(at), e, target);
    return target;
}

bool same_image(const PolygonRenderTarget& a, const PolygonRenderTarget& b) {
    for (std::size_t i = 0; i < a.hdrColor.size(); ++i)
        if (a.hdrColor[i].x != b.hdrColor[i].x || a.hdrColor[i].y != b.hdrColor[i].y || a.hdrColor[i].z != b.hdrColor[i].z) return false;
    return true;
}

void test_flat_and_tilted() {
    const RenderEnvironment e = sun_only();
    const PolygonRenderTarget plain = draw(matte(), e);
    VoxelMaterialDefinition flat = matte();
    flat.normalMap = constant_map(128, 128, 255);
    // 128 decodes to 0.0039, not 0: tiny but not exactly flat. Exactly flat is (127.5, 127.5, 255),
    // so compare against strength 0 for exactness and a flat map for closeness.
    VoxelMaterialDefinition off = flat;
    off.normalMapStrength = 0.0F;
    check(same_image(draw(off, e), plain), "strength 0: the image is exactly as without a map");
    const PolygonRenderTarget flatImage = draw(flat, e);
    double worst = 0.0;
    for (std::size_t i = 0; i < plain.hdrColor.size(); ++i) worst = std::max(worst, std::abs(static_cast<double>(flatImage.hdrColor[i].x - plain.hdrColor[i].x)));
    std::printf("flat map vs no map: worst difference %.5f (of %.3f)\n", worst, plain.hdrColor[0].x);
    check(worst < 0.01 * plain.hdrColor[0].x, "a flat map leaves the image as it is");

    // Constant tilt on the floor's top face (y projection: world = (tx, tz, ty)).
    VoxelMaterialDefinition tilted = matte();
    tilted.normalMap = constant_map(204, 128, 230);
    tilted.normalMapStrength = 1.5F;
    const PolygonRenderTarget image = draw(tilted, e);
    const Float3 t{204 / 127.5F - 1.0F, 128 / 127.5F - 1.0F, 230 / 127.5F - 1.0F};
    const Float3 bent = normalize(Float3{t.x * 1.5F, t.z, t.y * 1.5F});
    const double want = 0.6 * 2.0 * std::max(0.0F, dot(bent, normalize(e.sunDirection)));
    const std::size_t centre = 24U * 48U + 24U;
    std::printf("tilted map: pixel %.5f, exact %.5f (untilted %.5f); normal pass (%.3f %.3f %.3f)\n", image.hdrColor[centre].x, want,
                plain.hdrColor[centre].x, image.passes.normal[centre].x, image.passes.normal[centre].y, image.passes.normal[centre].z);
    check(std::abs(image.hdrColor[centre].x - want) < 1e-4 * want, "a tilted map lights the floor as the tilted normal");
    check(std::abs(image.passes.normal[centre].x - bent.x) < 1e-5F && std::abs(image.passes.normal[centre].z - bent.z) < 1e-5F,
          "the normal pass shows the bent normal");
    double spread = 0.0;
    for (std::size_t i = 0; i < image.hdrColor.size(); ++i)
        spread = std::max(spread, std::abs(static_cast<double>(image.hdrColor[i].x - image.hdrColor[centre].x)));
    check(spread < 1e-4, "a constant map is constant over the floor");
}

void test_tiling_and_motion() {
    VoxelMaterialDefinition bumps = matte();
    bumps.normalMap = bumpy_map();
    bumps.normalMapTileMeters = 0.8F;
    // Repeats every tile (object-space metres).
    double worst = 0.0;
    for (float x = 0.0F; x < 0.8F; x += 0.07F)
        for (float z = 0.0F; z < 0.8F; z += 0.05F) {
            const Float3 a = voxel_normal_map_normal(bumps, {x, 0.1F, z}, {0, 1, 0});
            const Float3 b = voxel_normal_map_normal(bumps, {x + 0.8F, 0.1F, z - 1.6F}, {0, 1, 0});
            worst = std::max(worst, static_cast<double>(length(subtract(a, b))));
        }
    check(worst < 1e-4, "the projection repeats every tile");
    // Side faces use their own projections and stay in front of the face.
    for (const Float3 n : {Float3{1, 0, 0}, Float3{-1, 0, 0}, Float3{0, 0, 1}, Float3{0, 0, -1}, Float3{0, -1, 0}})
        for (float s = 0.0F; s < 0.8F; s += 0.11F) {
            const Float3 bent = voxel_normal_map_normal(bumps, {s, 0.3F * s, 0.7F * s}, n);
            check(dot(bent, n) > 0.04F && std::abs(length(bent) - 1.0F) < 1e-4F, "bent normals stay unit length and in front of the face");
        }

    const RenderEnvironment e = sun_only();
    const PolygonRenderTarget here = draw(bumps, e);
    double low = 1e9, high = 0.0;
    for (const Float4& c : here.hdrColor) { low = std::min(low, static_cast<double>(c.x)); high = std::max(high, static_cast<double>(c.x)); }
    std::printf("bumpy floor: pixel range %.4f .. %.4f\n", low, high);
    check(high > 1.3 * low, "a bumpy map varies the shading");
    // Move the object (and the camera with it) by a non-tile amount: same image, since the map is
    // projected in the object's own space.
    RigidTransform moved;
    moved.position = {3.0F, 0.0F, -5.0F};
    const PolygonRenderTarget there = draw(bumps, e, moved, {3.3F, 1.0F, -4.8F});
    double diff = 0.0;
    for (std::size_t i = 0; i < here.hdrColor.size(); ++i) diff = std::max(diff, std::abs(static_cast<double>(here.hdrColor[i].x - there.hdrColor[i].x)));
    std::printf("moved object, moved camera: worst difference %.2e\n", diff);
    check(diff < 1e-4, "the map moves with the object");
}

void test_dvox(const fs::path& dir) {
    CookedVoxelAsset plain(21);
    plain.materials.resize(2);
    plain.materials[1].name = "Stone";
    plain.object.set_voxel({0, 0, 0}, 1);
    std::string error;
    check(write_dvox(dir / "plain.dvox", plain, {}, &error), "plain dvox: " + error);
    CookedVoxelAsset mapped(22);
    mapped.materials = plain.materials;
    mapped.object.set_voxel({0, 0, 0}, 1);
    mapped.materials[1].normalMap = bumpy_map();
    mapped.materials[1].normalMapTileMeters = 0.4F;
    mapped.materials[1].normalMapStrength = 0.75F;
    mapped.materials[1].normalMapSharpness = 12.0F;
    check(write_dvox(dir / "mapped.dvox", mapped, {}, &error), "mapped dvox: " + error);
    const auto minor = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        in.seekg(6);
        unsigned char b[2]{};
        in.read(reinterpret_cast<char*>(b), 2);
        return b[0] | (b[1] << 8U);
    };
    check(minor(dir / "plain.dvox") == 2, "no maps: DVOX v1.2 as before");
    check(minor(dir / "mapped.dvox") == 5, "normal maps need DVOX v1.5");
    const auto back = read_dvox(dir / "mapped.dvox");
    check(back.success, "mapped read: " + back.error);
    if (back.success) {
        const VoxelMaterialDefinition& m = back.asset.materials[1];
        check(m.normalMap && m.normalMap->rgba8 == mapped.materials[1].normalMap->rgba8 && m.normalMap->name == "bumps" &&
                  m.normalMap->width == 32U && m.normalMap->contentHash == mapped.materials[1].normalMap->contentHash &&
                  m.normalMapTileMeters == 0.4F && m.normalMapStrength == 0.75F && m.normalMapSharpness == 12.0F &&
                  !back.asset.materials[0].normalMap,
              "DVOX normal map round-trip");
    }
    CookedVoxelAsset bad(23);
    bad.materials = mapped.materials;
    bad.object.set_voxel({0, 0, 0}, 1);
    bad.materials[1].normalMapSharpness = 0.0F;
    check(!write_dvox(dir / "bad.dvox", bad, {}, &error), "out-of-range parameters are not written");
}

// --- a minimal PNG writer (stored deflate) for the import test ---
std::uint32_t crc32(const std::vector<std::uint8_t>& data, std::size_t begin) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = begin; i < data.size(); ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}
void be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::uint8_t>(v >> s));
}
void chunk(std::vector<std::uint8_t>& png, const char* type, const std::vector<std::uint8_t>& data) {
    be32(png, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    png.insert(png.end(), body.begin(), body.end());
    be32(png, crc32(body, 0));
}
std::vector<std::byte> png_rgba(std::uint32_t w, std::uint32_t h, const std::vector<std::uint8_t>& rgba) {
    std::vector<std::uint8_t> raw;
    for (std::uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + y * w * 4, rgba.begin() + (y + 1) * w * 4);
    }
    std::vector<std::uint8_t> z{0x78, 0x01, 0x01, static_cast<std::uint8_t>(raw.size()), static_cast<std::uint8_t>(raw.size() >> 8U),
                                static_cast<std::uint8_t>(~raw.size()), static_cast<std::uint8_t>(~raw.size() >> 8U)};
    z.insert(z.end(), raw.begin(), raw.end());
    std::uint32_t a = 1, b = 0;
    for (const std::uint8_t v : raw) { a = (a + v) % 65521U; b = (b + a) % 65521U; }
    be32(z, (b << 16U) | a);
    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> ihdr;
    be32(ihdr, w);
    be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    std::vector<std::byte> out(png.size());
    for (std::size_t i = 0; i < png.size(); ++i) out[i] = static_cast<std::byte>(png[i]);
    return out;
}

void test_png_import(const fs::path& dir) {
    std::vector<std::uint8_t> rgba;
    for (int i = 0; i < 3 * 2; ++i) rgba.insert(rgba.end(), {static_cast<std::uint8_t>(40 * i), 128, static_cast<std::uint8_t>(250 - i), 255});
    const auto bytes = png_rgba(3U, 2U, rgba);
    {
        std::ofstream out(dir / "stone_normal.png", std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    std::string error;
    const auto map = import_voxel_normal_map(dir / "stone_normal.png", &error);
    check(map != nullptr, "PNG import: " + error);
    if (map)
        check(map->width == 3U && map->height == 2U && map->rgba8 == rgba && map->name == "stone_normal",
              "PNG texels come through unchanged (no sRGB conversion)");
    // DirectX-style sources (green = down) convert at import: the flip inverts the green
    // channel only, before the map and its content hash are built.
    const auto flipped = import_voxel_normal_map(dir / "stone_normal.png", &error, true);
    check(flipped != nullptr, "flipped PNG import: " + error);
    if (map && flipped) {
        std::vector<std::uint8_t> want = rgba;
        for (std::size_t i = 1; i < want.size(); i += 4) want[i] = static_cast<std::uint8_t>(255U - want[i]);
        check(flipped->rgba8 == want, "flip inverts the green channel only (R/B/A untouched)");
        check(flipped->contentHash != map->contentHash, "the content hash covers the flipped texels");
        const auto decoded = decode_voxel_normal_map(bytes, "stone_normal", &error, true);
        check(decoded && decoded->rgba8 == flipped->rgba8 && decoded->contentHash == flipped->contentHash,
              "decode with flip matches import with flip");
        // The OpenGL twin of the same surface (green already up) imports to the identical map.
        std::vector<std::uint8_t> twin = rgba;
        for (std::size_t i = 1; i < twin.size(); i += 4) twin[i] = static_cast<std::uint8_t>(255U - twin[i]);
        const auto twinBytes = png_rgba(3U, 2U, twin);
        {
            std::ofstream out(dir / "stone_normal_gl.png", std::ios::binary);
            out.write(reinterpret_cast<const char*>(twinBytes.data()), static_cast<std::streamsize>(twinBytes.size()));
        }
        const auto gl = import_voxel_normal_map(dir / "stone_normal_gl.png", &error);
        check(gl && gl->rgba8 == flipped->rgba8 && gl->contentHash == flipped->contentHash,
              "a flipped DirectX map is identical to its OpenGL twin");
    }
    check(!import_voxel_normal_map(dir / "missing.png", &error) && !error.empty(), "a missing file is an error");
    const std::vector<std::byte> junk(16, std::byte{7});
    check(!decode_voxel_normal_map(junk, "junk", &error), "undecodable bytes are an error");
    check(!make_voxel_normal_map("big", 5000U, 1U, std::vector<std::uint8_t>(5000U * 4U), &error), "over 4096 per side is refused");
    check(!make_voxel_normal_map("short", 2U, 2U, std::vector<std::uint8_t>(15U), &error), "a texel count mismatch is refused");
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "dve_voxel_normal_maps_test";
    fs::create_directories(dir);
    test_flat_and_tilted();
    test_tiling_and_motion();
    test_dvox(dir);
    test_png_import(dir);
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (g_failures == 0) std::printf("dve_voxel_normal_maps_tests: PASS\n");
    return g_failures == 0 ? 0 : 1;
}
