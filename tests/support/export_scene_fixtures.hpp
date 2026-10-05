#pragma once

// Small, deterministic content for the editor-scene export tests and the dve_player export
// sample: a synthetic TrueType font (one triangular glyph for 'A', as in test_text3d.cpp), a
// closed box polygon asset (.dmesh) and a one-primitive Gabor blob. Header-only on purpose.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "dve/asset_cooker.hpp"
#include "dve/gabor_volume.hpp"
#include "dve/polygon_asset.hpp"
#include "dve/text3d.hpp"

namespace dve::test_fixtures {

using FontBytes = std::vector<std::uint8_t>;

inline void font_u16(FontBytes& b, std::uint16_t v) { b.push_back(static_cast<std::uint8_t>(v >> 8U)); b.push_back(static_cast<std::uint8_t>(v)); }
inline void font_i16(FontBytes& b, std::int16_t v) { font_u16(b, static_cast<std::uint16_t>(v)); }
inline void font_u32(FontBytes& b, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) b.push_back(static_cast<std::uint8_t>(v >> shift));
}
inline void font_set16(FontBytes& b, std::size_t o, std::uint16_t v) { b[o] = static_cast<std::uint8_t>(v >> 8U); b[o + 1] = static_cast<std::uint8_t>(v); }
inline void font_set32(FontBytes& b, std::size_t o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[o + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
}
inline std::uint32_t font_tag(const char* s) {
    return (static_cast<std::uint32_t>(s[0]) << 24U) | (static_cast<std::uint32_t>(s[1]) << 16U) |
           (static_cast<std::uint32_t>(s[2]) << 8U) | static_cast<std::uint32_t>(s[3]);
}

// 1000 units per em; 'A' is the triangle (0,0) (500,1000) (1000,0), advance 1100.
inline FontBytes make_test_font() {
    std::map<std::string, FontBytes> tables;
    FontBytes head(54, 0); font_set16(head, 18, 1000); font_set16(head, 50, 1); tables["head"] = head;
    FontBytes maxp; font_u32(maxp, 0x00010000U); font_u16(maxp, 2); tables["maxp"] = maxp;
    FontBytes hhea(36, 0); font_set32(hhea, 0, 0x00010000U); font_set16(hhea, 4, 800);
    font_set16(hhea, 6, static_cast<std::uint16_t>(-200)); font_set16(hhea, 8, 200); font_set16(hhea, 34, 2);
    tables["hhea"] = hhea;
    FontBytes hmtx; font_u16(hmtx, 500); font_i16(hmtx, 0); font_u16(hmtx, 1100); font_i16(hmtx, 0); tables["hmtx"] = hmtx;
    FontBytes glyf;
    font_i16(glyf, 1); font_i16(glyf, 0); font_i16(glyf, 0); font_i16(glyf, 1000); font_i16(glyf, 1000);
    font_u16(glyf, 2); font_u16(glyf, 0);
    glyf.push_back(1); glyf.push_back(1); glyf.push_back(1);
    font_i16(glyf, 0); font_i16(glyf, 500); font_i16(glyf, 500);
    font_i16(glyf, 0); font_i16(glyf, 1000); font_i16(glyf, -1000);
    tables["glyf"] = glyf;
    FontBytes loca; font_u32(loca, 0); font_u32(loca, 0); font_u32(loca, static_cast<std::uint32_t>(glyf.size())); tables["loca"] = loca;
    FontBytes cmap;
    font_u16(cmap, 0); font_u16(cmap, 1); font_u16(cmap, 3); font_u16(cmap, 1); font_u32(cmap, 12);
    font_u16(cmap, 4); font_u16(cmap, 32); font_u16(cmap, 0); font_u16(cmap, 4); font_u16(cmap, 4); font_u16(cmap, 1); font_u16(cmap, 0);
    font_u16(cmap, 65); font_u16(cmap, 0xFFFF); font_u16(cmap, 0);
    font_u16(cmap, 65); font_u16(cmap, 0xFFFF);
    font_i16(cmap, -64); font_i16(cmap, 1);
    font_u16(cmap, 0); font_u16(cmap, 0);
    tables["cmap"] = cmap;
    const auto count = static_cast<std::uint16_t>(tables.size());
    FontBytes out(12U + static_cast<std::size_t>(count) * 16U, 0);
    font_set32(out, 0, 0x00010000U); font_set16(out, 4, count);
    std::size_t record = 12, cursor = out.size();
    for (const auto& [name, data] : tables) {
        while (cursor % 4U) { out.push_back(0); ++cursor; }
        font_set32(out, record, font_tag(name.c_str()));
        font_set32(out, record + 4, 0);
        font_set32(out, record + 8, static_cast<std::uint32_t>(cursor));
        font_set32(out, record + 12, static_cast<std::uint32_t>(data.size()));
        out.insert(out.end(), data.begin(), data.end());
        cursor += data.size();
        record += 16;
    }
    return out;
}

// Cooks `text` (only 'A' and spaces exist in the font) with the synthetic font.
inline Text3DCookResult cook_test_text(const std::filesystem::path& scratchFolder, const std::string& text,
                                       const Text3DStyle& style, std::uint64_t objectId) {
    std::filesystem::create_directories(scratchFolder);
    const auto fontPath = scratchFolder / "synthetic_test_font.ttf";
    const FontBytes bytes = make_test_font();
    {
        std::ofstream out(fontPath, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    Text3DCookOptions options;
    options.objectId = objectId;
    options.style = style;
    return cook_text3d(fontPath, text, options);
}

// Closed axis-aligned box centred on the origin, one material, outward normals.
inline CookedPolygonAsset make_box_polygon(Float3 halfExtents, Float4 color, std::uint64_t objectId) {
    ImportedScene scene;
    scene.name = "Box";
    ImportedMaterial material;
    material.name = "Box";
    material.baseColorFactor = color;
    material.metallicFactor = 0.0F;
    material.roughnessFactor = 0.8F;
    scene.materials.push_back(material);
    ImportedMesh mesh;
    mesh.name = "Box";
    const Float3 h = halfExtents;
    struct Face { Float3 normal; Float3 u; Float3 v; };
    const Face faces[6] = {
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}}};
    for (const Face& face : faces) {
        const auto base = static_cast<std::uint32_t>(mesh.vertices.size());
        const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (const auto& c : corners) {
            const Float3 p{(face.normal.x + face.u.x * c[0] + face.v.x * c[1]) * h.x,
                           (face.normal.y + face.u.y * c[0] + face.v.y * c[1]) * h.y,
                           (face.normal.z + face.u.z * c[0] + face.v.z * c[1]) * h.z};
            mesh.vertices.push_back({p, face.normal, {(c[0] + 1) * 0.5F, (c[1] + 1) * 0.5F}, color, {}});
        }
        mesh.triangles.push_back({{base, base + 1U, base + 2U}, 0, 0});
        mesh.triangles.push_back({{base, base + 2U, base + 3U}, 0, 0});
    }
    scene.meshes.push_back(mesh);
    ImportedNode node;
    node.name = "Box";
    node.mesh = 0;
    node.worldTransform = Matrix4::identity();
    scene.nodes.push_back(node);
    scene.roots.push_back(0);
    PolygonCookOptions options;
    options.objectId = objectId;
    return cook_polygon_scene(scene, options);
}

// One isotropic primitive at the origin; `densityMultiplier` scales the whole field.
inline GaborVolumeAsset make_gabor_blob(float radiusScale, float densityMultiplier, Float3 tint) {
    GaborVolumeAsset volume;
    volume.name = "Blob";
    volume.material.densityMultiplier = densityMultiplier;
    volume.material.albedoTint = tint;
    GaborVolumePrimitive primitive;
    primitive.scale = {radiusScale, radiusScale, radiusScale};
    primitive.opacity = 1.0F;
    primitive.frequency = 0.0F;
    volume.primitives.push_back(primitive);
    volume.recompute_bounds_and_hash();
    return volume;
}

} // namespace dve::test_fixtures
