#include "dve/editor_sprite_authoring.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

[[nodiscard]] std::uint32_t parse_u32(std::string_view value, std::string_view option) {
    std::size_t consumed = 0U;
    const unsigned long parsed = std::stoul(std::string(value), &consumed, 10);
    if (consumed != value.size() || parsed > 0xFFFFFFFFUL) {
        throw std::runtime_error(std::string(option) + " requires a 32-bit unsigned integer");
    }
    return static_cast<std::uint32_t>(parsed);
}

[[nodiscard]] dve::SpritePaletteIndexPolicy parse_palette_policy(std::string_view value) {
    if (value == "exact") return dve::SpritePaletteIndexPolicy::Exact;
    if (value == "nearest") return dve::SpritePaletteIndexPolicy::Nearest;
    if (value == "error") return dve::SpritePaletteIndexPolicy::Error;
    throw std::runtime_error("--palette-policy must be exact, nearest, or error");
}

void print_usage() {
    std::cerr
        << "Usage: dve_cook_sprite <source.dvesprite> --atlas <output.png> "
           "--asset <cooked.dvesprite> [options]\n"
        << "Options:\n"
        << "  --project-root <path>       Root used to resolve the source texture\n"
        << "  --manifest <path>           Dependency manifest output\n"
        << "  --texture-ref <path>        Texture reference stored in the cooked asset\n"
        << "  --palette <source>          Convert the atlas with a .dvepalette asset\n"
        << "  --palette-output <path>     Published .dvepalette output\n"
        << "  --palette-ref <path>        Palette reference stored in the cooked asset\n"
        << "  --palette-bank <index>      Source/default bank (default 0)\n"
        << "  --palette-policy <mode>     exact, nearest, or error (default error)\n"
        << "  --alpha-threshold <0..255>  Transparent-index alpha threshold\n"
        << "  --unmatched-index <0..255> Exact-mode fallback index\n"
        << "  --max-width <1..16384>      Shelf-pack width (default 1024)\n"
        << "  --padding <0..64>           Padding around frames (default 1)\n"
        << "  --power-of-two              Round atlas dimensions to powers of two\n"
        << "  --no-extrude                Disable edge extrusion\n"
        << "  --force                     Publish even when dependency outputs match\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) {
            print_usage();
            return 2;
        }
        const std::filesystem::path sourceAsset = argv[1];
        std::filesystem::path projectRoot;
        std::filesystem::path paletteSource;
        std::filesystem::path paletteOutput;
        std::string paletteReference;
        dve::SpritePaletteIndexSettings paletteIndexing;
        dve::editor::SpriteAtlasPublishSettings settings;
        for (int index = 2; index < argc; ++index) {
            const std::string_view argument = argv[index];
            auto require_value = [&](std::string_view option) -> std::string_view {
                if (index + 1 >= argc) {
                    throw std::runtime_error(std::string(option) + " requires a value");
                }
                return argv[++index];
            };
            if (argument == "--project-root") projectRoot = require_value(argument);
            else if (argument == "--atlas") settings.atlasPath = require_value(argument);
            else if (argument == "--asset") settings.cookedAssetPath = require_value(argument);
            else if (argument == "--manifest") settings.dependencyManifestPath = require_value(argument);
            else if (argument == "--texture-ref") settings.textureAssetReference = require_value(argument);
            else if (argument == "--palette") paletteSource = require_value(argument);
            else if (argument == "--palette-output") paletteOutput = require_value(argument);
            else if (argument == "--palette-ref") paletteReference = require_value(argument);
            else if (argument == "--palette-bank") {
                paletteIndexing.bank = parse_u32(require_value(argument), argument);
            } else if (argument == "--palette-policy") {
                paletteIndexing.policy = parse_palette_policy(require_value(argument));
            } else if (argument == "--alpha-threshold") {
                const std::uint32_t value = parse_u32(require_value(argument), argument);
                if (value > 255U) throw std::runtime_error("--alpha-threshold must be in [0, 255]");
                paletteIndexing.alphaThreshold = static_cast<std::uint8_t>(value);
            } else if (argument == "--unmatched-index") {
                const std::uint32_t value = parse_u32(require_value(argument), argument);
                if (value > 255U) throw std::runtime_error("--unmatched-index must be in [0, 255]");
                paletteIndexing.unmatchedIndex = static_cast<std::uint16_t>(value);
            } else if (argument == "--max-width") {
                settings.packing.maximumWidth = parse_u32(require_value(argument), argument);
            } else if (argument == "--padding") {
                settings.packing.paddingPixels = parse_u32(require_value(argument), argument);
            } else if (argument == "--power-of-two") settings.packing.powerOfTwo = true;
            else if (argument == "--no-extrude") settings.packing.extrudeEdges = false;
            else if (argument == "--force") settings.skipIfUnchanged = false;
            else if (argument == "--help" || argument == "-h") {
                print_usage();
                return 0;
            } else {
                throw std::runtime_error("unknown option: " + std::string(argument));
            }
        }
        if (settings.atlasPath.empty() || settings.cookedAssetPath.empty()) {
            throw std::runtime_error("--atlas and --asset are required");
        }
        if (!paletteSource.empty()) {
            if (paletteOutput.empty())
                throw std::runtime_error("--palette-output is required with --palette");
            const dve::SpritePaletteReadResult palette = dve::read_dvepalette(paletteSource);
            if (!palette) throw std::runtime_error(palette.error);
            dve::editor::SpriteIndexedAtlasPublishSettings indexed;
            indexed.palette = palette.asset;
            indexed.indexing = paletteIndexing;
            indexed.palettePath = paletteOutput;
            indexed.paletteAssetReference = paletteReference;
            settings.indexedPalette = std::move(indexed);
        } else if (!paletteOutput.empty() || !paletteReference.empty()) {
            throw std::runtime_error("--palette-output and --palette-ref require --palette");
        }
        dve::editor::SpriteAuthoringWorkspace workspace;
        std::string error;
        if (!workspace.open_asset(sourceAsset, projectRoot, &error)) {
            throw std::runtime_error(error);
        }
        dve::editor::SpriteAtlasPublishResult result;
        if (!workspace.publish_packed_atlas(settings, result, &error)) {
            throw std::runtime_error(error);
        }
        std::cout << "{\n"
                  << "  \"status\": \"" << (result.upToDate ? "up_to_date" : "published") << "\",\n"
                  << "  \"dependency_key\": " << result.dependencyKey << ",\n"
                  << "  \"atlas_content_hash\": " << result.atlasContentHash << ",\n"
                  << "  \"cooked_asset_hash\": " << result.cookedAssetHash << ",\n"
                  << "  \"indexed_palette\": "
                  << (settings.indexedPalette ? "true" : "false") << ",\n"
                  << "  \"palette_content_hash\": " << result.paletteContentHash << ",\n"
                  << "  \"palette_exact_pixels\": "
                  << result.paletteDiagnostics.exactPixels << ",\n"
                  << "  \"palette_nearest_pixels\": "
                  << result.paletteDiagnostics.nearestPixels << ",\n"
                  << "  \"palette_unmatched_pixels\": "
                  << result.paletteDiagnostics.unmatchedPixels << ",\n"
                  << "  \"atlas_width\": " << result.atlasWidth << ",\n"
                  << "  \"atlas_height\": " << result.atlasHeight << ",\n"
                  << "  \"frames\": " << result.frameCount << "\n"
                  << "}\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_cook_sprite: " << exception.what() << '\n';
        return 1;
    }
}
