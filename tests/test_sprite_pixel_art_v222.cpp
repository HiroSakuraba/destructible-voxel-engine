#include "dve/sprite_pixel_art.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace dve;
using namespace dve::editor;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

SpritePaletteAsset make_palette() {
    SpritePaletteAsset palette;
    palette.name = "v2.22 Test Palette";
    palette.transparentIndex = 0U;
    SpritePaletteBank bank;
    bank.name = "Default";
    bank.colors = {{0,0,0,0}, {255,255,255,255}, {255,64,64,255}, {64,160,255,255},
                   {32,32,48,255}, {255,220,80,255}};
    palette.banks.push_back(std::move(bank));
    palette.recompute_hash();
    return palette;
}

void run() {
    SpritePixelArtSession session;
    std::string error;
    require(session.new_document("Sun Pilot", 8U, 8U, SpritePixelStorage::Indexed8,
                                 make_palette(), &error), error);
    session.set_index(2U);
    require(session.draw_line(1, 1, 6, 6, &error), error);
    session.set_index(3U);
    require(session.draw_rectangle({0, 0, 8U, 8U}, false, &error), error);
    require(session.add_layer("Highlights", &error), error);
    session.set_index(5U);
    require(session.paint(3, 4, &error), error);
    require(session.paint(4, 4, &error), error);
    require(session.set_layer_opacity(1U, 224U, &error), error);
    require(session.set_selection({2, 2, 4U, 4U}, &error), error);
    require(session.flip_selection_x(&error), error);
    require(session.rotate_selection_90(true, &error), error);
    require(session.move_selection(1, -1, true, &error), error);
    require(session.add_frame("blink", true, &error), error);
    require(session.set_frame_duration(1U, 0.15F, &error), error);
    session.set_index(1U);
    require(session.flood_fill(3, 3, &error), error);
    std::vector<std::byte> onion;
    require(session.onion_rgba8(1U, true, false, onion, &error), error);
    require(onion.size() == 8U * 8U * 4U, "onion preview has the wrong size");
    const std::uint64_t editedHash = session.document().contentHash;
    require(session.undo(), "pixel-art undo failed");
    require(session.redo(), "pixel-art redo failed");
    require(session.document().contentHash == editedHash, "pixel-art redo did not restore the document");

    const auto root = std::filesystem::temp_directory_path() / "dve_v222_pixel_art_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto source = root / "pilot.dvepixel";
    require(session.save(source, &error), error);
    SpritePixelArtDocument reopened;
    require(read_dvepixel(source, reopened, &error), error);
    require(reopened.contentHash == session.document().contentHash, "pixel-art round trip changed the hash");

    SpritePixelArtPublishSettings settings;
    settings.imagePath = root / "pilot_index.png";
    settings.spritePath = root / "pilot.dvesprite";
    settings.palettePath = root / "pilot.dvepalette";
    settings.imageAssetReference = "pilot_index.png";
    settings.paletteAssetReference = "pilot.dvepalette";
    settings.columns = 2U;
    SpritePixelArtPublishResult published;
    require(session.publish(settings, published, &error), error);
    require(published.sheetWidth == 16U && published.sheetHeight == 8U,
            "pixel-art sprite sheet layout is wrong");
    const auto sprite = read_dvesprite(settings.spritePath);
    require(static_cast<bool>(sprite), sprite.error);
    require(sprite.asset.frames.size() == 2U && sprite.asset.clips.front().frames.size() == 2U,
            "pixel-art publication omitted frames");
    require(sprite.asset.paletteAsset == "pilot.dvepalette",
            "indexed pixel-art publication omitted its palette reference");
    require(std::filesystem::file_size(settings.imagePath) > 32U,
            "pixel-art publication did not write a PNG");
    std::filesystem::remove_all(root);
}
}

int main() {
    try {
        run();
        std::cout << "dve_v222_sprite_pixel_art_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
