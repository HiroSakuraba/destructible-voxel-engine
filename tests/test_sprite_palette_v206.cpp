#include "dve/sprite2d.hpp"
#include "dve/sprite_palette.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {
using namespace dve;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

SpritePaletteAsset palette_asset() {
    SpritePaletteAsset palette;
    palette.name = "Hero palettes";
    palette.transparentIndex = 0U;
    palette.banks = {
        {"day", {{0U, 0U, 0U, 0U}, {255U, 0U, 0U, 255U},
                 {0U, 255U, 0U, 255U}, {0U, 0U, 255U, 255U}}},
        {"night", {{0U, 0U, 0U, 0U}, {255U, 255U, 0U, 255U},
                   {0U, 255U, 255U, 255U}, {255U, 0U, 255U, 255U}}},
    };
    palette.cycles = {
        {"energy", 1U, 3U, 2U, 0, SpritePaletteCycleDirection::Forward},
    };
    palette.recompute_hash();
    return palette;
}

constexpr std::uint64_t kTestFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kTestFnvPrime = 1099511628211ULL;

void legacy_hash_byte(std::uint64_t& hash, std::uint8_t value) {
    hash ^= value;
    hash *= kTestFnvPrime;
}

template<class T> void legacy_hash_integer(std::uint64_t& hash, T value) {
    using Unsigned = std::make_unsigned_t<T>;
    const Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        legacy_hash_byte(hash, static_cast<std::uint8_t>(
            (bits >> (index * 8U)) & static_cast<Unsigned>(0xFFU)));
    }
}

void legacy_hash_string(std::uint64_t& hash, std::string_view value) {
    legacy_hash_integer(hash, static_cast<std::uint64_t>(value.size()));
    for (char character : value) {
        legacy_hash_byte(hash, static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
    }
}

void legacy_hash_float(std::uint64_t& hash, float value) {
    legacy_hash_integer(hash, std::bit_cast<std::uint32_t>(value));
}

std::uint64_t legacy_sprite_hash(const SpriteAsset& asset) {
    std::uint64_t hash = kTestFnvOffset;
    legacy_hash_string(hash, asset.name);
    legacy_hash_string(hash, asset.textureAsset);
    legacy_hash_integer(hash, asset.textureWidth);
    legacy_hash_integer(hash, asset.textureHeight);
    legacy_hash_float(hash, asset.pixelsPerWorldUnit);
    legacy_hash_byte(hash, static_cast<std::uint8_t>(asset.sampling));
    legacy_hash_integer(hash, asset.materialId);
    legacy_hash_integer(hash, asset.paletteBank);
    legacy_hash_integer(hash, static_cast<std::uint64_t>(asset.frames.size()));
    for (const SpriteFrame& frame : asset.frames) {
        legacy_hash_string(hash, frame.name);
        legacy_hash_integer(hash, frame.atlasRect.x);
        legacy_hash_integer(hash, frame.atlasRect.y);
        legacy_hash_integer(hash, frame.atlasRect.width);
        legacy_hash_integer(hash, frame.atlasRect.height);
        legacy_hash_integer(hash, frame.sourceWidth);
        legacy_hash_integer(hash, frame.sourceHeight);
        legacy_hash_integer(hash, frame.sourceOffsetX);
        legacy_hash_integer(hash, frame.sourceOffsetY);
        legacy_hash_float(hash, frame.pivotPixels.x);
        legacy_hash_float(hash, frame.pivotPixels.y);
        legacy_hash_float(hash, frame.durationSeconds);
        legacy_hash_string(hash, frame.event);
    }
    legacy_hash_integer(hash, static_cast<std::uint64_t>(asset.clips.size()));
    for (const SpriteClip& clip : asset.clips) {
        legacy_hash_string(hash, clip.name);
        legacy_hash_byte(hash, static_cast<std::uint8_t>(clip.loopMode));
        legacy_hash_float(hash, clip.playbackRate);
        legacy_hash_integer(hash, static_cast<std::uint64_t>(clip.frames.size()));
        for (SpriteFrameIndex frame : clip.frames) legacy_hash_integer(hash, frame);
    }
    return hash;
}

SpriteAsset indexed_sprite() {
    SpriteAsset asset;
    asset.name = "Indexed hero";
    asset.textureAsset = "hero_indices.png";
    asset.textureWidth = 2U;
    asset.textureHeight = 2U;
    asset.pixelsPerWorldUnit = 16.0F;
    asset.sampling = SpriteSampling::Nearest;
    asset.paletteAsset = "hero.dvepalette";
    asset.paletteBank = 0U;
    asset.frames = {
        {"idle", {0U, 0U, 2U, 2U}, 2U, 2U, 0, 0, {1.0F, 0.0F}, 0.1F, ""},
    };
    asset.clips = {{"idle", SpriteLoopMode::Loop, 1.0F, {0U}}};
    asset.recompute_hash();
    return asset;
}

void test_palette_validation_hash_and_codec() {
    SpritePaletteAsset palette = palette_asset();
    std::string error;
    require(palette.validate(&error), error);
    const std::uint64_t hash = palette.contentHash;
    palette.banks[1].colors[1].r = 240U;
    require(sprite_palette_content_hash(palette) != hash,
            "palette hash ignored bank color content");
    palette = palette_asset();

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "dve_v206_palette_test.dvepalette";
    require(write_dvepalette(path, palette, &error), error);
    const SpritePaletteReadResult read = read_dvepalette(path);
    require(read && read.asset.contentHash == palette.contentHash &&
            read.asset.banks.size() == 2U && read.asset.cycles.size() == 1U &&
            read.asset.transparentIndex == 0U,
            read.error.empty() ? "palette round trip changed content" : read.error);
    require(!read_dvepalette(path, 8U), "palette byte limit was ignored");
    {
        std::ofstream stream(path, std::ios::binary | std::ios::app);
        stream << "trailing";
    }
    require(!read_dvepalette(path), "trailing palette data was accepted");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    SpritePaletteAsset invalid = palette_asset();
    invalid.banks[1].colors.pop_back();
    require(!invalid.validate(), "mismatched palette bank lengths were accepted");
    invalid = palette_asset();
    invalid.cycles.push_back(
        {"overlap", 2U, 3U, 1U, 0, SpritePaletteCycleDirection::Reverse});
    require(!invalid.validate(), "overlapping palette cycles were accepted");
    invalid = palette_asset();
    invalid.banks[0].colors[0].a = 1U;
    require(!invalid.validate(), "opaque transparent-index entry was accepted");
    invalid = palette_asset();
    invalid.cycles = {{"bad_alpha_cycle", 0U, 1U, 1U, 0,
                       SpritePaletteCycleDirection::Forward}};
    require(!invalid.validate(), "cycle track was allowed to move the transparent index");
}

void test_cycle_swap_and_indexing() {
    const SpritePaletteAsset palette = palette_asset();
    require(sample_sprite_palette_cycle_shift(palette.cycles[0], 0U) == 0U &&
            sample_sprite_palette_cycle_shift(palette.cycles[0], 2U) == 1U &&
            sample_sprite_palette_cycle_shift(palette.cycles[0], 6U) == 0U,
            "forward palette-cycle sampling is incorrect");
    const std::uint64_t maximumClock = std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t reducedClock =
        ((maximumClock / palette.cycles[0].ticksPerStep) % 3U) *
        palette.cycles[0].ticksPerStep;
    require(sample_sprite_palette_cycle_shift(palette.cycles[0], maximumClock) ==
                sample_sprite_palette_cycle_shift(palette.cycles[0], reducedClock),
            "palette-cycle sampling overflowed at the maximum clock value");
    SpritePaletteCycleTrack reverse = palette.cycles[0];
    reverse.direction = SpritePaletteCycleDirection::Reverse;
    require(sample_sprite_palette_cycle_shift(reverse, 2U) == 2U,
            "reverse palette-cycle sampling is incorrect");
    SpritePaletteCycleTrack ping = palette.cycles[0];
    ping.direction = SpritePaletteCycleDirection::PingPong;
    require(sample_sprite_palette_cycle_shift(ping, 0U) == 0U &&
            sample_sprite_palette_cycle_shift(ping, 2U) == 1U &&
            sample_sprite_palette_cycle_shift(ping, 4U) == 2U &&
            sample_sprite_palette_cycle_shift(ping, 6U) == 1U,
            "ping-pong palette-cycle sampling is incorrect");

    const std::array<SpritePaletteSwap, 1> swaps{{{1U, 2U}}};
    SpritePalettePacket packet;
    std::string error;
    require(resolve_sprite_palette(palette, {0U, 2U, swaps}, packet, &error), error);
    require(packet.colors.size() == 4U &&
            packet.colors[1] == SpriteColor8{255U, 0U, 0U, 255U} &&
            packet.colors[2] == SpriteColor8{255U, 0U, 0U, 255U} &&
            packet.stateHash != 0U,
            "palette cycle then per-instance swap resolved incorrectly");
    require(!validate_sprite_palette_swaps(
                std::array<SpritePaletteSwap, 2>{{{1U, 2U}, {1U, 3U}}}, 4U),
            "duplicate palette swap source was accepted");
    require(!validate_sprite_palette_swaps(
                palette, std::array<SpritePaletteSwap, 1>{{{0U, 1U}}}),
            "palette swap was allowed to remap the transparent index");

    const std::array<std::byte, 16> rgba{
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255},
        std::byte{0}, std::byte{255}, std::byte{0}, std::byte{255},
        std::byte{250}, std::byte{3}, std::byte{0}, std::byte{255},
    };
    SpritePaletteIndexSettings settings;
    settings.policy = SpritePaletteIndexPolicy::Exact;
    settings.unmatchedIndex = 3U;
    settings.alphaThreshold = 0U;
    std::vector<std::byte> indices;
    SpritePaletteIndexDiagnostics diagnostics;
    require(index_sprite_rgba8(4U, 1U, rgba, palette, settings,
                               indices, diagnostics, &error), error);
    require(indices.size() == 4U && std::to_integer<std::uint8_t>(indices[0]) == 0U &&
            std::to_integer<std::uint8_t>(indices[1]) == 1U &&
            std::to_integer<std::uint8_t>(indices[2]) == 2U &&
            std::to_integer<std::uint8_t>(indices[3]) == 3U &&
            diagnostics.transparentPixels == 1U && diagnostics.exactPixels == 2U &&
            diagnostics.unmatchedPixels == 1U,
            "exact palette indexing or diagnostics are incorrect");

    settings.policy = SpritePaletteIndexPolicy::Error;
    require(!index_sprite_rgba8(4U, 1U, rgba, palette, settings,
                                indices, diagnostics, &error) && indices.empty(),
            "error palette policy accepted an unmatched source color");
    settings.policy = SpritePaletteIndexPolicy::Nearest;
    require(index_sprite_rgba8(4U, 1U, rgba, palette, settings,
                               indices, diagnostics, &error), error);
    require(std::to_integer<std::uint8_t>(indices[3]) == 1U &&
            diagnostics.nearestPixels == 1U && diagnostics.maximumDistanceSquared == 34U,
            "nearest palette indexing is not deterministic");

    std::vector<std::byte> transport;
    require(encode_sprite_indices_rgba8(indices, palette, transport, &error), error);
    require(transport.size() == 16U && transport[0] == std::byte{0} &&
            transport[3] == std::byte{0} && transport[4] == std::byte{1} &&
            transport[7] == std::byte{255},
            "palette index transport encoding is incorrect");
}

void test_sprite_runtime_palette_packets() {
    SpriteRuntime runtime;
    std::string error;
    require(!runtime.register_asset(1U, indexed_sprite(), &error),
            "indexed sprite registered before its palette dependency");
    require(runtime.register_palette("hero.dvepalette", palette_asset(), &error), error);
    require(runtime.register_asset(1U, indexed_sprite(), &error), error);

    SpriteInstanceDesc first;
    first.asset = 1U;
    first.clip = "idle";
    first.transform = make_rigid_transform({}, {});
    require(runtime.bind(10U, first, &error), error);

    SpriteInstanceDesc second = first;
    second.paletteBankOverride = 1U;
    second.paletteSwaps = {{1U, 2U}};
    require(runtime.bind(20U, second, &error), error);

    SpriteRenderList list = runtime.build_render_list();
    require(list.items.size() == 2U && list.batches.size() == 2U &&
            list.palettes.size() == 2U &&
            list.items[0].palettePacket != kInvalidSpritePalettePacket &&
            list.items[1].palettePacket != kInvalidSpritePalettePacket &&
            list.items[0].paletteStateHash != list.items[1].paletteStateHash,
            "runtime did not produce distinct palette packets and batches");
    const std::uint64_t initialState = list.items[0].paletteStateHash;
    runtime.advance_palette_ticks(1U);
    list = runtime.build_render_list();
    require(list.items[0].paletteStateHash == initialState,
            "palette state changed before its fixed-step cycle boundary");
    runtime.advance_palette_ticks(1U);
    list = runtime.build_render_list();
    require(list.items[0].paletteStateHash != initialState,
            "deterministic palette clock did not advance cycle state");
    require(runtime.set_palette_bank(10U, 1U, &error), error);
    require(runtime.set_palette_swaps(10U, {{1U, 3U}}, &error), error);
    require(!runtime.set_palette_swaps(10U, {{9U, 1U}}, &error),
            "out-of-range runtime palette swap was accepted");
    require(!runtime.set_palette_swaps(10U, {{0U, 1U}}, &error),
            "runtime accepted a transparent-index palette swap");

    const std::uint64_t beforeWallTick = runtime.palette_clock_ticks();
    runtime.tick(1.0F / 240.0F);
    require(runtime.palette_clock_ticks() == beforeWallTick + 1U,
            "wall-clock tick did not feed the fixed palette clock");
    SpriteRuntime saturationRuntime;
    saturationRuntime.tick(std::numeric_limits<float>::max());
    require(saturationRuntime.palette_clock_ticks() ==
                std::numeric_limits<std::uint64_t>::max(),
            "palette wall-clock conversion did not saturate safely");
    require(!runtime.unregister_palette("hero.dvepalette"),
            "runtime removed a palette still referenced by a sprite asset");
    require(runtime.unbind(10U) && runtime.unbind(20U) && runtime.unregister_asset(1U) &&
            runtime.unregister_palette("hero.dvepalette"),
            "palette runtime teardown failed");
}

void test_indexed_sprite_codec() {
    SpriteAsset asset = indexed_sprite();
    std::string error;
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "dve_v206_indexed_sprite.dvesprite";
    require(write_dvesprite(path, asset, &error), error);
    const SpriteAssetReadResult read = read_dvesprite(path);
    require(read && read.asset.paletteAsset == "hero.dvepalette" &&
            read.asset.paletteBank == 0U && read.asset.contentHash == asset.contentHash,
            read.error.empty() ? "indexed sprite codec lost palette metadata" : read.error);
    std::error_code ec;
    std::filesystem::remove(path, ec);

    SpriteAsset legacy = asset;
    legacy.name = "Legacy hero";
    legacy.textureAsset = "legacy.png";
    legacy.paletteAsset.clear();
    legacy.paletteBank = 0U;
    legacy.materialId = 7U;
    legacy.frames[0].durationSeconds = 0.125F;
    legacy.contentHash = 0U;
    const std::uint64_t legacyHash = legacy_sprite_hash(legacy);
    const std::filesystem::path legacyPath =
        std::filesystem::temp_directory_path() / "dve_v206_legacy_sprite.dvesprite";
    {
        std::ofstream stream(legacyPath, std::ios::binary | std::ios::trunc);
        stream << "DVE_SPRITE 1\n"
               << "asset \"Legacy hero\" \"legacy.png\" 2 2 16 0 7 0 1 1\n"
               << "frame \"idle\" 0 0 2 2 2 2 0 0 1 0 0.125 \"\"\n"
               << "clip \"idle\" 1 1 1 0\n"
               << "hash " << legacyHash << "\n";
    }
    const SpriteAssetReadResult legacyRead = read_dvesprite(legacyPath);
    require(legacyRead && legacyRead.asset.paletteAsset.empty() &&
                legacyRead.asset.contentHash == sprite_asset_content_hash(legacyRead.asset),
            legacyRead.error.empty() ? "legacy v1 sprite was not normalized to v2" : legacyRead.error);
    std::filesystem::remove(legacyPath, ec);

    asset.sampling = SpriteSampling::Linear;
    asset.contentHash = 0U;
    require(!asset.validate(), "linear filtering was accepted for an indexed sprite");
}

} // namespace

int main() {
    try {
        test_palette_validation_hash_and_codec();
        test_cycle_swap_and_indexing();
        test_sprite_runtime_palette_packets();
        test_indexed_sprite_codec();
        std::cout << "dve_v206_sprite_palette_tests: PASS\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "dve_v206_sprite_palette_tests: FAIL: " << exception.what() << '\n';
        return 1;
    }
}
