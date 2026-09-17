#include "dve/editor_sprite_authoring.hpp"

#include <png.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace dve;
using namespace dve::editor;

[[noreturn]] void fail_test(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void require(bool condition, const std::string& message) {
    if (!condition) fail_test(message);
}

std::vector<std::byte> make_source_png() {
    constexpr std::uint32_t width = 4U;
    constexpr std::uint32_t height = 2U;
    const SpriteColor8 transparent{0U, 0U, 0U, 0U};
    const SpriteColor8 red{255U, 0U, 0U, 255U};
    const SpriteColor8 green{0U, 255U, 0U, 255U};
    const std::array<SpriteColor8, width * height> pixels{
        transparent, red, green, green,
        red, red, green, transparent,
    };
    std::vector<std::byte> rgba;
    rgba.reserve(pixels.size() * 4U);
    for (const SpriteColor8 color : pixels) {
        rgba.push_back(static_cast<std::byte>(color.r));
        rgba.push_back(static_cast<std::byte>(color.g));
        rgba.push_back(static_cast<std::byte>(color.b));
        rgba.push_back(static_cast<std::byte>(color.a));
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = width;
    image.height = height;
    image.format = PNG_FORMAT_RGBA;
    png_alloc_size_t byteCount = 0U;
    require(png_image_write_to_memory(&image, nullptr, &byteCount, 0, rgba.data(), 0, nullptr) != 0,
            "measure source PNG");
    std::vector<std::byte> encoded(static_cast<std::size_t>(byteCount));
    require(png_image_write_to_memory(
                &image, encoded.data(), &byteCount, 0, rgba.data(), 0, nullptr) != 0,
            "encode source PNG");
    encoded.resize(static_cast<std::size_t>(byteCount));
    return encoded;
}

void write_bytes(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(stream), "open test file for writing");
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(stream), "write complete test file");
}

std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(static_cast<bool>(stream), "open published file for comparison");
    stream.seekg(0, std::ios::end);
    const std::streamoff end = stream.tellg();
    require(end >= 0, "measure published file");
    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(stream) || stream.eof(), "read published file");
    return bytes;
}

SpritePaletteAsset make_palette(bool includeGreen = true) {
    SpritePaletteAsset palette;
    palette.name = includeGreen ? "hero" : "hero_bad";
    palette.transparentIndex = 0U;
    palette.banks = {
        {"base", {
            {0U, 0U, 0U, 0U},
            {255U, 0U, 0U, 255U},
            includeGreen ? SpriteColor8{0U, 255U, 0U, 255U}
                         : SpriteColor8{0U, 0U, 255U, 255U},
        }},
        {"night", {
            {0U, 0U, 0U, 0U},
            {160U, 48U, 48U, 255U},
            {24U, 112U, 96U, 255U},
        }},
    };
    palette.cycles = {{"pulse", 1U, 2U, 3U, 0, SpritePaletteCycleDirection::PingPong}};
    palette.recompute_hash();
    return palette;
}

void test_indexed_publication() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "dve_v206_indexed_sprite_cook";
    std::filesystem::remove_all(root);
    const std::filesystem::path sourcePath = root / "hero_source.png";
    write_bytes(sourcePath, make_source_png());

    SpriteAuthoringWorkspace workspace;
    std::string error;
    require(workspace.create_from_texture(
                sourcePath, "hero", sourcePath.generic_string(), 16.0F, &error),
            "create sprite workspace: " + error);
    SpriteGridSliceSettings slicing;
    slicing.cellWidth = 2U;
    slicing.cellHeight = 2U;
    slicing.columns = 2U;
    slicing.rows = 1U;
    slicing.trimTransparent = false;
    require(workspace.slice_grid(slicing, &error), "slice source atlas: " + error);
    const std::filesystem::path sourceAssetPath = root / "hero_source.dvesprite";
    require(workspace.save_asset(sourceAssetPath, &error),
            "save source-space sprite for CLI coverage: " + error);

    SpriteAtlasPublishSettings settings;
    settings.packing.maximumWidth = 16U;
    settings.packing.paddingPixels = 1U;
    settings.packing.powerOfTwo = true;
    settings.packing.extrudeEdges = true;
    settings.atlasPath = root / "hero_indices.png";
    settings.cookedAssetPath = root / "hero.dvesprite";
    settings.dependencyManifestPath = root / "hero.dvesprite.cook";
    settings.textureAssetReference = "hero_indices.png";
    SpriteIndexedAtlasPublishSettings indexed;
    indexed.palette = make_palette();
    indexed.palettePath = root / "hero.dvepalette";
    indexed.paletteAssetReference = "hero.dvepalette";
    indexed.indexing.policy = SpritePaletteIndexPolicy::Error;
    indexed.indexing.bank = 0U;
    indexed.indexing.alphaThreshold = 0U;
    indexed.indexing.unmatchedIndex = 0U;
    settings.indexedPalette = indexed;

    SpriteAtlasPublishResult first;
    require(workspace.publish_packed_atlas(settings, first, &error),
            "publish indexed sprite: " + error);
    require(!first.upToDate && first.paletteContentHash != 0U,
            "first indexed cook publishes a palette dependency");
    require(first.paletteDiagnostics.unmatchedPixels == 0U &&
                first.paletteDiagnostics.nearestPixels == 0U &&
                first.paletteDiagnostics.exactPixels > 0U,
            "error-policy indexed cook used exact palette matches");
    require(std::filesystem::exists(settings.atlasPath) &&
                std::filesystem::exists(settings.cookedAssetPath) &&
                std::filesystem::exists(settings.dependencyManifestPath) &&
                std::filesystem::exists(indexed.palettePath),
            "indexed cook published all four transaction outputs");

    const SpriteAssetReadResult cooked = read_dvesprite(settings.cookedAssetPath);
    require(cooked && cooked.asset.paletteAsset == "hero.dvepalette" &&
                cooked.asset.paletteBank == 0U &&
                cooked.asset.sampling == SpriteSampling::Nearest,
            "cooked sprite carries the indexed-palette runtime contract");
    const SpritePaletteReadResult publishedPalette = read_dvepalette(indexed.palettePath);
    require(publishedPalette &&
                publishedPalette.asset.contentHash == first.paletteContentHash &&
                publishedPalette.asset.banks.size() == 2U,
            "published palette round-trips with banks and cycles");

    SpriteDecodedImage atlas;
    require(load_sprite_image(settings.atlasPath, atlas, &error),
            "decode indexed transport atlas: " + error);
    require(atlas.width == first.atlasWidth && atlas.height == first.atlasHeight,
            "indexed transport atlas dimensions match cook metadata");
    bool sawTransparent = false;
    bool sawRed = false;
    bool sawGreen = false;
    for (std::size_t offset = 0U; offset < atlas.rgba8.size(); offset += 4U) {
        const std::uint8_t index = static_cast<std::uint8_t>(atlas.rgba8[offset]);
        const std::uint8_t greenChannel = static_cast<std::uint8_t>(atlas.rgba8[offset + 1U]);
        const std::uint8_t blueChannel = static_cast<std::uint8_t>(atlas.rgba8[offset + 2U]);
        const std::uint8_t alpha = static_cast<std::uint8_t>(atlas.rgba8[offset + 3U]);
        require(index < 3U && greenChannel == 0U && blueChannel == 0U,
                "transport PNG stores one palette index in the red channel");
        require(alpha == (index == 0U ? 0U : 255U),
                "transport PNG alpha follows the declared transparent index");
        sawTransparent = sawTransparent || index == 0U;
        sawRed = sawRed || index == 1U;
        sawGreen = sawGreen || index == 2U;
    }
    require(sawTransparent && sawRed && sawGreen,
            "transport atlas retains every authored palette index");

    SpriteAtlasPublishResult second;
    require(workspace.publish_packed_atlas(settings, second, &error),
            "repeat indexed sprite cook: " + error);
    require(second.upToDate && second.dependencyKey == first.dependencyKey,
            "unchanged indexed cook is an incremental no-op");

    settings.indexedPalette->palette.banks[1U].colors[1U] = {96U, 64U, 160U, 255U};
    settings.indexedPalette->palette.recompute_hash();
    SpriteAtlasPublishResult paletteChanged;
    require(workspace.publish_packed_atlas(settings, paletteChanged, &error),
            "republish changed palette: " + error);
    require(!paletteChanged.upToDate && paletteChanged.dependencyKey != first.dependencyKey &&
                paletteChanged.paletteContentHash != first.paletteContentHash,
            "palette-only changes invalidate the dependency key");

    const std::array<std::filesystem::path, 4> outputs{
        settings.atlasPath, settings.cookedAssetPath, settings.dependencyManifestPath,
        settings.indexedPalette->palettePath,
    };
    std::array<std::vector<std::byte>, 4> stableBytes;
    for (std::size_t index = 0U; index < outputs.size(); ++index) {
        stableBytes[index] = read_bytes(outputs[index]);
    }
    settings.indexedPalette->palette = make_palette(false);
    error.clear();
    SpriteAtlasPublishResult rejected;
    require(!workspace.publish_packed_atlas(settings, rejected, &error) && !error.empty(),
            "error indexing policy rejects an unmatched source color");
    for (std::size_t index = 0U; index < outputs.size(); ++index) {
        require(read_bytes(outputs[index]) == stableBytes[index],
                "failed indexed cook preserves every prior publication output");
    }

    if (std::getenv("DVE_KEEP_V206_COOK_ARTIFACTS") == nullptr) {
        std::filesystem::remove_all(root);
    } else {
        std::cout << "kept_v206_cook_artifacts=" << root.generic_string() << '\n';
    }
}

} // namespace

int main() {
    test_indexed_publication();
    std::cout << "dve_v206_sprite_palette_cook_tests: PASS\n";
    return 0;
}
