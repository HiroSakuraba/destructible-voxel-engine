#include "dve/editor_text3d.hpp"
#include "dve/editor_viewport.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string("CHECK failed: ") + #x); } while (false)

namespace {
using namespace dve;
using namespace dve::editor;
using Bytes = std::vector<std::uint8_t>;

void u16(Bytes& b, std::uint16_t v) { b.push_back(static_cast<std::uint8_t>(v >> 8U)); b.push_back(static_cast<std::uint8_t>(v)); }
void i16(Bytes& b, std::int16_t v) { u16(b, static_cast<std::uint16_t>(v)); }
void u32(Bytes& b, std::uint32_t v) { b.push_back(static_cast<std::uint8_t>(v >> 24U)); b.push_back(static_cast<std::uint8_t>(v >> 16U)); b.push_back(static_cast<std::uint8_t>(v >> 8U)); b.push_back(static_cast<std::uint8_t>(v)); }
void set16(Bytes& b, std::size_t o, std::uint16_t v) { b[o] = static_cast<std::uint8_t>(v >> 8U); b[o + 1U] = static_cast<std::uint8_t>(v); }
void set32(Bytes& b, std::size_t o, std::uint32_t v) { b[o] = static_cast<std::uint8_t>(v >> 24U); b[o + 1U] = static_cast<std::uint8_t>(v >> 16U); b[o + 2U] = static_cast<std::uint8_t>(v >> 8U); b[o + 3U] = static_cast<std::uint8_t>(v); }
std::uint32_t tag(const char* s) { return (static_cast<std::uint32_t>(s[0]) << 24U) | (static_cast<std::uint32_t>(s[1]) << 16U) | (static_cast<std::uint32_t>(s[2]) << 8U) | static_cast<std::uint32_t>(s[3]); }

Bytes make_test_font() {
    std::map<std::string, Bytes> tables;
    Bytes head(54, 0); set16(head, 18, 1000); set16(head, 50, 1); tables["head"] = head;
    Bytes maxp; u32(maxp, 0x00010000U); u16(maxp, 2); tables["maxp"] = maxp;
    Bytes hhea(36, 0); set32(hhea, 0, 0x00010000U); set16(hhea, 4, 800); set16(hhea, 6, static_cast<std::uint16_t>(-200)); set16(hhea, 8, 200); set16(hhea, 34, 2); tables["hhea"] = hhea;
    Bytes hmtx; u16(hmtx, 500); i16(hmtx, 0); u16(hmtx, 1100); i16(hmtx, 0); tables["hmtx"] = hmtx;
    Bytes glyf;
    i16(glyf, 1); i16(glyf, 0); i16(glyf, 0); i16(glyf, 1000); i16(glyf, 1000);
    u16(glyf, 2); u16(glyf, 0); glyf.insert(glyf.end(), {1, 1, 1});
    i16(glyf, 0); i16(glyf, 500); i16(glyf, 500); i16(glyf, 0); i16(glyf, 1000); i16(glyf, -1000);
    tables["glyf"] = glyf;
    Bytes loca; u32(loca, 0); u32(loca, 0); u32(loca, static_cast<std::uint32_t>(glyf.size())); tables["loca"] = loca;
    Bytes cmap;
    u16(cmap, 0); u16(cmap, 1); u16(cmap, 3); u16(cmap, 1); u32(cmap, 12);
    u16(cmap, 4); u16(cmap, 32); u16(cmap, 0); u16(cmap, 4); u16(cmap, 4); u16(cmap, 1); u16(cmap, 0);
    u16(cmap, 65); u16(cmap, 0xFFFF); u16(cmap, 0); u16(cmap, 65); u16(cmap, 0xFFFF);
    i16(cmap, -64); i16(cmap, 1); u16(cmap, 0); u16(cmap, 0); tables["cmap"] = cmap;
    const auto count = static_cast<std::uint16_t>(tables.size());
    Bytes out(12U + static_cast<std::size_t>(count) * 16U, 0);
    set32(out, 0, 0x00010000U); set16(out, 4, count);
    std::size_t record = 12U;
    std::size_t cursor = out.size();
    for (const auto& [name, data] : tables) {
        while (cursor % 4U != 0U) { out.push_back(0); ++cursor; }
        set32(out, record, tag(name.c_str()));
        set32(out, record + 8U, static_cast<std::uint32_t>(cursor));
        set32(out, record + 12U, static_cast<std::uint32_t>(data.size()));
        out.insert(out.end(), data.begin(), data.end());
        cursor += data.size();
        record += 16U;
    }
    return out;
}

std::filesystem::path make_project() {
    static std::uint64_t index = 0U;
    const auto root = std::filesystem::temp_directory_path() /
        ("dve_editor_text3d_" + std::to_string(++index));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "assets/fonts");
    std::filesystem::create_directories(root / "scenes");
    const auto font = make_test_font();
    std::ofstream stream(root / "assets/fonts/Default.ttf", std::ios::binary);
    stream.write(reinterpret_cast<const char*>(font.data()), static_cast<std::streamsize>(font.size()));
    stream.close();
    std::ofstream(root / "assets/fonts/OFL.txt") << "Synthetic test font license\n";
    return root;
}

void run() {
    const auto root = make_project();
    EditorWorkspace workspace(EditorDocument("Text Project"));
    EditorText3DAuthoringSession session;
    Text3DCookOptions options;
    options.style.emSizeMeters = 0.75F;
    options.style.extrusionDepthMeters = 0.15F;
    options.style.faceMaterialId = 7U;
    options.style.sideMaterialId = 8U;
    options.style.horizontalBands = 4U;
    options.style.verticalBands = 4U;
    CHECK(session.begin_create(root, "assets/fonts/Default.ttf", "A", options));
    CHECK(session.preview());
    CHECK(session.preview().dependency.packageReady);
    session.set_output_asset_path("assets/text/title.dtext");
    CHECK(session.commit(workspace, "Title", make_rigid_transform({1.0F, 2.0F, 0.0F}, {})).success);
    CHECK(workspace.selected_object().has_value());
    const EditorObjectId id = *workspace.selected_object();
    const EditorObject* object = workspace.document().find_object(id);
    CHECK(object != nullptr && object->text3d.has_value());
    CHECK(object->voxels->occupied_voxel_count() == 0U);
    CHECK(object->text3d->style.faceMaterialId == 7U);
    CHECK(object->sourceAsset == std::filesystem::path("assets/text/title.dtext"));
    CHECK(std::filesystem::is_regular_file(root / object->sourceAsset));

    const EditorObjectBounds bounds = object_world_bounds(*object);
    CHECK(bounds.valid);
    CHECK(bounds.maximum.x > bounds.minimum.x && bounds.maximum.y > bounds.minimum.y);
    EditorCamera camera;
    frame_camera_on_bounds(camera, bounds);
    const UiRect viewport{0, 0, 800, 600};
    const auto items = build_text3d_draw_list(workspace.document(), camera, viewport, id);
    CHECK(items.size() == 1U && items.front().screenBounds.width > 0 && items.front().selected);
    const ViewportRay ray = make_viewport_ray(camera, viewport, 400.0F, 300.0F);
    const auto pick = pick_editor_document(workspace.document(), ray);
    CHECK(pick.has_value() && pick->objectId == id && pick->material == 7U);

    CHECK(workspace.commands().undo(workspace.document()).success);
    CHECK(workspace.document().find_object(id) == nullptr);
    CHECK(workspace.commands().redo(workspace.document()).success);
    CHECK(workspace.document().find_object(id) != nullptr);

    const auto oldHash = workspace.document().find_object(id)->text3d->contentHash;
    CHECK(session.begin_edit(root, *workspace.document().find_object(id)));
    session.set_text("AA");
    CHECK(session.refresh());
    CHECK(session.commit(workspace).success);
    CHECK(workspace.document().find_object(id)->text3d->textUtf8 == "AA");
    CHECK(workspace.document().find_object(id)->text3d->contentHash != oldHash);
    CHECK(workspace.commands().undo(workspace.document()).success);
    CHECK(workspace.document().find_object(id)->text3d->textUtf8 == "A");

    const auto save = workspace.document().save_transactional(root / "scenes/text_scene.dvescene");
    CHECK(save.success);
    CHECK(std::filesystem::is_regular_file(save.revisionDirectory / (std::to_string(id) + ".dtext")));
    std::string error;
    const auto loaded = EditorDocument::load(save.manifestPath, &error);
    CHECK(loaded.has_value());
    const EditorObject* loadedText = loaded->find_object(id);
    CHECK(loadedText != nullptr && loadedText->text3d.has_value());
    CHECK(loadedText->text3d->contentHash == workspace.document().find_object(id)->text3d->contentHash);
    CHECK(loadedText->textFontAsset == std::filesystem::path("assets/fonts/Default.ttf"));

    const auto packaged = package_text3d_font_dependencies(
        workspace.document(), root, root / "package", true);
    CHECK(packaged.success);
    CHECK(std::filesystem::is_regular_file(root / "package/assets/fonts/Default.ttf"));
    CHECK(std::filesystem::is_regular_file(root / "package/assets/fonts/OFL.txt"));

    std::filesystem::remove(root / "assets/fonts/OFL.txt");
    const auto missingLicense = inspect_text3d_font_dependency(root, "assets/fonts/Default.ttf");
    CHECK(missingLicense.exists && missingLicense.insideProject && !missingLicense.packageReady);
    const auto rejectedPackage = package_text3d_font_dependencies(
        workspace.document(), root, root / "package_without_license", true);
    CHECK(!rejectedPackage.success);
    std::filesystem::remove_all(root);
}
} // namespace

int main() {
    try {
        run();
        std::cout << "editor text3d tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
