#include "dve/sprite_palette_authoring.hpp"

#include <array>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void test_transactional_palette_authoring() {
    SpritePaletteAuthoringSession session;
    std::string error;
    require(session.create("Hero", 4U, &error), error);
    require(session.set_transparent_index(0U, &error), error);
    require(!session.set_color(0U, {1U, 2U, 3U, 255U}, &error),
            "transparent swatch accepted opaque alpha");
    require(session.set_color(1U, {255U, 0U, 0U, 255U}, &error), error);
    require(session.add_bank("Night", true, &error), error);
    require(session.asset().banks.size() == 2U && session.selection().bank == 1U,
            "palette bank duplication did not select the new bank");
    require(session.select_color(1U), "could not select an editable palette swatch");
    require(session.set_selected_color({0U, 0U, 255U, 255U}, &error), error);
    require(session.undo(&error), error);
    require(session.asset().banks[1U].colors[0U].a == 0U,
            "palette undo damaged transparent entry semantics");
    require(session.redo(&error), error);

    SpritePaletteCycleTrack cycle;
    cycle.name = "Glow";
    cycle.firstIndex = 1U;
    cycle.lastIndex = 3U;
    cycle.ticksPerStep = 2U;
    cycle.direction = SpritePaletteCycleDirection::Forward;
    require(session.add_cycle(cycle, &error), error);
    session.seek_preview(2U);
    SpritePalettePacket packet;
    require(session.resolve_preview(packet, &error), error);
    require(packet.colors.size() == 4U && packet.colors[0U].a == 0U,
            "palette preview did not preserve the transparent swatch");
}

void test_import_save_reload_and_conflict() {
    SpritePaletteAuthoringSession session;
    const std::array<std::byte, 24U> pixels{
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255},
        std::byte{0}, std::byte{255}, std::byte{0}, std::byte{255},
        std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255},
        std::byte{0}, std::byte{0}, std::byte{255}, std::byte{255},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
    };
    SpritePaletteImportResult imported;
    std::string error;
    require(session.import_rgba8("Imported", 3U, 2U, pixels, 0U, imported, &error), error);
    require(imported.uniqueColors == 4U && imported.transparentPixels == 2U &&
            session.asset().transparentIndex == 0U,
            "row-major palette import diagnostics are incorrect");

    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "dve_palette_authoring_v207";
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    std::filesystem::create_directories(directory, ec);
    require(!ec, "could not create palette authoring test directory");
    const std::filesystem::path path = directory / "hero.dvepalette";
    require(session.save(path, &error), error);
    require(!session.dirty(), "saved palette remained dirty");

    SpritePaletteAuthoringSession reopened;
    require(reopened.open(path, &error), error);
    require(reopened.asset().contentHash == session.asset().contentHash,
            "palette authoring reopen changed the asset hash");

    SpritePaletteAsset external = reopened.asset();
    external.banks[0U].colors[1U] = {12U, 34U, 56U, 255U};
    external.recompute_hash();
    require(write_dvepalette(path, external, &error), error);
    const auto reloaded = reopened.poll_external_change(true);
    require(reloaded.state == SpritePaletteExternalChange::Reloaded &&
            reopened.asset().contentHash == external.contentHash,
            "clean palette document did not hot reload an external change");

    require(reopened.set_color(1U, {99U, 88U, 77U, 255U}, &error), error);
    external.banks[0U].colors[1U] = {1U, 2U, 3U, 255U};
    external.recompute_hash();
    require(write_dvepalette(path, external, &error), error);
    const auto conflict = reopened.poll_external_change(true);
    require(conflict.state == SpritePaletteExternalChange::Conflict && reopened.dirty(),
            "dirty palette document did not protect local edits from hot reload");
    std::filesystem::remove_all(directory, ec);
}

} // namespace

int main() {
    try {
        test_transactional_palette_authoring();
        test_import_save_reload_and_conflict();
        std::cout << "dve_v207_sprite_palette_authoring_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_v207_sprite_palette_authoring_tests: FAIL: "
                  << exception.what() << '\n';
        return 1;
    }
}
