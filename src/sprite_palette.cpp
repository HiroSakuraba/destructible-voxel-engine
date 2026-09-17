#include "dve/sprite_palette.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <type_traits>

namespace dve {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::size_t kMaximumBanks = 256U;
constexpr std::size_t kMaximumCycles = 256U;
constexpr std::size_t kMaximumDiagnosticColors = 32U;

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kFnvPrime;
}

template<class T> void hash_integer(std::uint64_t& hash, T value) noexcept {
    using Unsigned = std::make_unsigned_t<T>;
    const Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t index = 0U; index < sizeof(T); ++index) {
        hash_byte(hash, static_cast<std::uint8_t>(
            (bits >> (index * 8U)) & static_cast<Unsigned>(0xFFU)));
    }
}

void hash_string(std::uint64_t& hash, std::string_view value) noexcept {
    hash_integer(hash, static_cast<std::uint64_t>(value.size()));
    for (const char raw : value)
        hash_byte(hash, static_cast<std::uint8_t>(static_cast<unsigned char>(raw)));
}

[[nodiscard]] bool write_atomic(
    const std::filesystem::path& path, std::string_view bytes, std::string* error) {
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return fail(error, "could not create sprite palette directory: " + ec.message());
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return fail(error, "could not open temporary sprite palette");
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, ec);
            return fail(error, "could not write complete sprite palette");
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
    }
    if (!ec) return true;
    std::filesystem::remove(temporary, ec);
    return fail(error, "could not publish sprite palette: " + ec.message());
}

[[nodiscard]] std::uint32_t color_distance_squared(
    SpriteColor8 left, SpriteColor8 right) noexcept {
    const auto square = [](int value) noexcept { return static_cast<std::uint32_t>(value * value); };
    return square(static_cast<int>(left.r) - static_cast<int>(right.r)) +
        square(static_cast<int>(left.g) - static_cast<int>(right.g)) +
        square(static_cast<int>(left.b) - static_cast<int>(right.b)) +
        square(static_cast<int>(left.a) - static_cast<int>(right.a));
}

[[nodiscard]] std::int64_t positive_mod(std::int64_t value, std::int64_t modulus) noexcept {
    if (modulus <= 0) return 0;
    const std::int64_t remainder = value % modulus;
    return remainder < 0 ? remainder + modulus : remainder;
}

[[nodiscard]] std::uint64_t palette_state_hash(
    const SpritePaletteAsset& palette,
    std::uint32_t bank,
    std::span<const SpriteColor8> colors) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_integer(hash, palette.contentHash == 0U ? sprite_palette_content_hash(palette)
                                                : palette.contentHash);
    hash_integer(hash, bank);
    for (const SpriteColor8 color : colors) {
        hash_byte(hash, color.r); hash_byte(hash, color.g);
        hash_byte(hash, color.b); hash_byte(hash, color.a);
    }
    return hash;
}

} // namespace

bool SpritePaletteAsset::validate(std::string* error) const {
    if (name.empty() || name.size() > 255U || banks.empty() || banks.size() > kMaximumBanks)
        return fail(error, "sprite palette name or bank count is invalid");
    const std::size_t entries = banks.front().colors.size();
    if (entries == 0U || entries > kMaximumSpritePaletteEntries)
        return fail(error, "sprite palette entry count is invalid");
    if (transparentIndex && *transparentIndex >= entries)
        return fail(error, "sprite palette transparent index is out of range");

    std::set<std::string, std::less<>> bankNames;
    for (const SpritePaletteBank& bank : banks) {
        if (bank.name.empty() || bank.name.size() > 255U ||
            !bankNames.insert(bank.name).second || bank.colors.size() != entries)
            return fail(error, "sprite palette bank is invalid");
        if (transparentIndex && bank.colors[*transparentIndex].a != 0U)
            return fail(error, "sprite palette transparent index must have alpha zero in every bank");
    }

    if (cycles.size() > kMaximumCycles)
        return fail(error, "sprite palette has too many cycle tracks");
    std::set<std::string, std::less<>> cycleNames;
    std::vector<bool> occupied(entries, false);
    for (const SpritePaletteCycleTrack& cycle : cycles) {
        if (cycle.name.empty() || cycle.name.size() > 255U ||
            !cycleNames.insert(cycle.name).second || cycle.firstIndex >= cycle.lastIndex ||
            cycle.lastIndex >= entries || cycle.ticksPerStep == 0U ||
            cycle.ticksPerStep > 86400000U ||
            static_cast<unsigned>(cycle.direction) >
                static_cast<unsigned>(SpritePaletteCycleDirection::PingPong))
            return fail(error, "sprite palette cycle track is invalid");
        if (transparentIndex && cycle.firstIndex <= *transparentIndex &&
            *transparentIndex <= cycle.lastIndex)
            return fail(error, "sprite palette cycles cannot move the transparent index");
        for (std::size_t index = cycle.firstIndex; index <= cycle.lastIndex; ++index) {
            if (occupied[index]) return fail(error, "sprite palette cycle ranges overlap");
            occupied[index] = true;
        }
    }
    return true;
}

void SpritePaletteAsset::recompute_hash() noexcept {
    contentHash = sprite_palette_content_hash(*this);
}

std::uint64_t sprite_palette_content_hash(const SpritePaletteAsset& palette) noexcept {
    std::uint64_t hash = kFnvOffset;
    hash_string(hash, palette.name);
    hash_byte(hash, palette.transparentIndex ? 1U : 0U);
    if (palette.transparentIndex) hash_integer(hash, *palette.transparentIndex);
    hash_integer(hash, static_cast<std::uint64_t>(palette.banks.size()));
    for (const SpritePaletteBank& bank : palette.banks) {
        hash_string(hash, bank.name);
        hash_integer(hash, static_cast<std::uint64_t>(bank.colors.size()));
        for (const SpriteColor8 color : bank.colors) {
            hash_byte(hash, color.r); hash_byte(hash, color.g);
            hash_byte(hash, color.b); hash_byte(hash, color.a);
        }
    }
    hash_integer(hash, static_cast<std::uint64_t>(palette.cycles.size()));
    for (const SpritePaletteCycleTrack& cycle : palette.cycles) {
        hash_string(hash, cycle.name);
        hash_integer(hash, cycle.firstIndex);
        hash_integer(hash, cycle.lastIndex);
        hash_integer(hash, cycle.ticksPerStep);
        hash_integer(hash, cycle.phaseSteps);
        hash_byte(hash, static_cast<std::uint8_t>(cycle.direction));
    }
    return hash;
}

const SpritePaletteBank* find_sprite_palette_bank(
    const SpritePaletteAsset& palette, std::string_view name) noexcept {
    const auto found = std::find_if(palette.banks.begin(), palette.banks.end(),
        [name](const SpritePaletteBank& bank) { return bank.name == name; });
    return found == palette.banks.end() ? nullptr : &*found;
}

bool write_dvepalette(
    const std::filesystem::path& path,
    const SpritePaletteAsset& palette,
    std::string* error) {
    if (!palette.validate(error)) return false;
    const std::uint64_t hash = sprite_palette_content_hash(palette);
    if (palette.contentHash != 0U && palette.contentHash != hash)
        return fail(error, "sprite palette content hash is stale");
    std::ostringstream stream;
    stream << "DVE_PALETTE 1\n";
    stream << "palette " << std::quoted(palette.name) << ' ' << palette.banks.size() << ' '
           << palette.entry_count() << ' ';
    if (palette.transparentIndex) stream << static_cast<int>(*palette.transparentIndex);
    else stream << -1;
    stream << ' ' << palette.cycles.size() << "\n";
    for (const SpritePaletteBank& bank : palette.banks) {
        stream << "bank " << std::quoted(bank.name);
        for (const SpriteColor8 color : bank.colors) {
            stream << ' ' << static_cast<unsigned>(color.r)
                   << ' ' << static_cast<unsigned>(color.g)
                   << ' ' << static_cast<unsigned>(color.b)
                   << ' ' << static_cast<unsigned>(color.a);
        }
        stream << "\n";
    }
    for (const SpritePaletteCycleTrack& cycle : palette.cycles) {
        stream << "cycle " << std::quoted(cycle.name) << ' ' << cycle.firstIndex << ' '
               << cycle.lastIndex << ' ' << cycle.ticksPerStep << ' ' << cycle.phaseSteps << ' '
               << static_cast<unsigned>(cycle.direction) << "\n";
    }
    stream << "hash " << hash << "\n";
    return write_atomic(path, stream.str(), error);
}

SpritePaletteReadResult read_dvepalette(
    const std::filesystem::path& path,
    std::uint64_t maximumBytes) {
    SpritePaletteReadResult result;
    std::error_code ec;
    const std::uint64_t bytes = std::filesystem::file_size(path, ec);
    if (ec || bytes > maximumBytes) {
        result.error = ec ? "could not stat sprite palette" : "sprite palette exceeds byte limit";
        return result;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { result.error = "could not open sprite palette"; return result; }
    std::string magic, token;
    unsigned version{};
    std::size_t bankCount{}, entryCount{}, cycleCount{};
    int transparent{};
    if (!(stream >> magic >> version) || magic != "DVE_PALETTE" || version != 1U ||
        !(stream >> token) || token != "palette" ||
        !(stream >> std::quoted(result.asset.name) >> bankCount >> entryCount >> transparent >> cycleCount) ||
        bankCount == 0U || bankCount > kMaximumBanks || entryCount == 0U ||
        entryCount > kMaximumSpritePaletteEntries || cycleCount > kMaximumCycles ||
        transparent < -1 || transparent >= static_cast<int>(entryCount)) {
        result.error = "unsupported or invalid sprite palette header";
        return result;
    }
    if (transparent >= 0) result.asset.transparentIndex = static_cast<std::uint16_t>(transparent);
    result.asset.banks.resize(bankCount);
    for (SpritePaletteBank& bank : result.asset.banks) {
        if (!(stream >> token) || token != "bank" || !(stream >> std::quoted(bank.name))) {
            result.error = "invalid sprite palette bank record";
            return result;
        }
        bank.colors.resize(entryCount);
        for (SpriteColor8& color : bank.colors) {
            unsigned r{}, g{}, b{}, a{};
            if (!(stream >> r >> g >> b >> a) || r > 255U || g > 255U || b > 255U || a > 255U) {
                result.error = "invalid sprite palette color";
                return result;
            }
            color = {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                     static_cast<std::uint8_t>(b), static_cast<std::uint8_t>(a)};
        }
    }
    result.asset.cycles.resize(cycleCount);
    for (SpritePaletteCycleTrack& cycle : result.asset.cycles) {
        unsigned direction{};
        if (!(stream >> token) || token != "cycle" ||
            !(stream >> std::quoted(cycle.name) >> cycle.firstIndex >> cycle.lastIndex >>
              cycle.ticksPerStep >> cycle.phaseSteps >> direction) ||
            direction > static_cast<unsigned>(SpritePaletteCycleDirection::PingPong)) {
            result.error = "invalid sprite palette cycle record";
            return result;
        }
        cycle.direction = static_cast<SpritePaletteCycleDirection>(direction);
    }
    if (!(stream >> token >> result.asset.contentHash) || token != "hash") {
        result.error = "missing sprite palette content hash";
        return result;
    }
    stream >> std::ws;
    if (!stream.eof()) { result.error = "trailing sprite palette data"; return result; }
    if (!result.asset.validate(&result.error)) return result;
    if (result.asset.contentHash != sprite_palette_content_hash(result.asset))
        result.error = "sprite palette content hash mismatch";
    return result;
}

bool validate_sprite_palette_swaps(
    std::span<const SpritePaletteSwap> swaps,
    std::size_t entryCount,
    std::string* error) {
    if (entryCount == 0U || entryCount > kMaximumSpritePaletteEntries)
        return fail(error, "sprite palette swap entry count is invalid");
    if (swaps.size() > entryCount)
        return fail(error, "sprite palette has too many swap entries");
    std::array<bool, kMaximumSpritePaletteEntries> seen{};
    for (const SpritePaletteSwap swap : swaps) {
        if (swap.sourceIndex >= entryCount || swap.destinationIndex >= entryCount)
            return fail(error, "sprite palette swap index is out of range");
        if (seen[swap.sourceIndex])
            return fail(error, "sprite palette swap source index is duplicated");
        seen[swap.sourceIndex] = true;
    }
    return true;
}

bool validate_sprite_palette_swaps(
    const SpritePaletteAsset& palette,
    std::span<const SpritePaletteSwap> swaps,
    std::string* error) {
    if (!palette.validate(error) ||
        !validate_sprite_palette_swaps(swaps, palette.entry_count(), error))
        return false;
    if (palette.transparentIndex) {
        for (const SpritePaletteSwap swap : swaps) {
            if (swap.sourceIndex == *palette.transparentIndex ||
                swap.destinationIndex == *palette.transparentIndex)
                return fail(error, "sprite palette swaps cannot remap the transparent index");
        }
    }
    return true;
}

std::uint32_t sample_sprite_palette_cycle_shift(
    const SpritePaletteCycleTrack& track,
    std::uint64_t clockTicks) noexcept {
    if (track.firstIndex >= track.lastIndex || track.ticksPerStep == 0U) return 0U;
    const std::uint64_t length = static_cast<std::uint64_t>(track.lastIndex) -
        static_cast<std::uint64_t>(track.firstIndex) + 1U;
    const std::uint64_t period = track.direction == SpritePaletteCycleDirection::PingPong
        ? 2U * (length - 1U) : length;
    const std::uint64_t clockPhase = (clockTicks / track.ticksPerStep) % period;
    const std::uint64_t authoredPhase = static_cast<std::uint64_t>(positive_mod(
        static_cast<std::int64_t>(track.phaseSteps), static_cast<std::int64_t>(period)));
    const std::uint64_t phase = (clockPhase + authoredPhase) % period;
    switch (track.direction) {
        case SpritePaletteCycleDirection::Forward:
            return static_cast<std::uint32_t>(phase);
        case SpritePaletteCycleDirection::Reverse:
            return static_cast<std::uint32_t>((period - phase) % period);
        case SpritePaletteCycleDirection::PingPong:
            return static_cast<std::uint32_t>(phase < length ? phase : period - phase);
    }
    return 0U;
}

bool resolve_sprite_palette(
    const SpritePaletteAsset& palette,
    const SpritePaletteResolveDesc& desc,
    SpritePalettePacket& out,
    std::string* error) {
    out = {};
    if (!palette.validate(error)) return false;
    if (desc.bank >= palette.banks.size())
        return fail(error, "sprite palette bank is out of range");
    const std::size_t entries = palette.entry_count();
    if (!validate_sprite_palette_swaps(palette, desc.swaps, error)) return false;

    std::vector<SpriteColor8> cycled = palette.banks[desc.bank].colors;
    for (const SpritePaletteCycleTrack& track : palette.cycles) {
        const std::uint32_t shift = sample_sprite_palette_cycle_shift(track, desc.clockTicks);
        const std::size_t first = track.firstIndex;
        const std::size_t length = static_cast<std::size_t>(track.lastIndex - track.firstIndex + 1U);
        const std::vector<SpriteColor8> source(
            cycled.begin() + static_cast<std::ptrdiff_t>(first),
            cycled.begin() + static_cast<std::ptrdiff_t>(first + length));
        for (std::size_t offset = 0U; offset < length; ++offset) {
            const std::size_t sourceOffset = (offset + length - (shift % length)) % length;
            cycled[first + offset] = source[sourceOffset];
        }
    }

    std::array<std::uint16_t, kMaximumSpritePaletteEntries> remap{};
    for (std::size_t index = 0U; index < entries; ++index)
        remap[index] = static_cast<std::uint16_t>(index);
    for (const SpritePaletteSwap swap : desc.swaps) remap[swap.sourceIndex] = swap.destinationIndex;

    out.bank = desc.bank;
    out.clockTicks = desc.clockTicks;
    out.paletteContentHash = palette.contentHash == 0U
        ? sprite_palette_content_hash(palette) : palette.contentHash;
    out.colors.resize(entries);
    for (std::size_t index = 0U; index < entries; ++index) out.colors[index] = cycled[remap[index]];
    out.stateHash = palette_state_hash(palette, desc.bank, out.colors);
    return true;
}

bool index_sprite_rgba8(
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::byte> rgba8,
    const SpritePaletteAsset& palette,
    const SpritePaletteIndexSettings& settings,
    std::vector<std::byte>& outIndices,
    SpritePaletteIndexDiagnostics& diagnostics,
    std::string* error) {
    outIndices.clear();
    diagnostics = {};
    if (!palette.validate(error)) return false;
    if (settings.bank >= palette.banks.size())
        return fail(error, "sprite palette indexing bank is out of range");
    if (settings.unmatchedIndex >= palette.entry_count())
        return fail(error, "sprite palette unmatched index is out of range");
    if (static_cast<unsigned>(settings.policy) > static_cast<unsigned>(SpritePaletteIndexPolicy::Error))
        return fail(error, "sprite palette index policy is invalid");
    const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
    if (width == 0U || height == 0U || pixels > std::numeric_limits<std::size_t>::max() ||
        rgba8.size() != static_cast<std::size_t>(pixels) * 4U)
        return fail(error, "sprite palette source image dimensions do not match RGBA8 payload");

    const auto& colors = palette.banks[settings.bank].colors;
    std::map<std::array<std::uint8_t, 4>, std::uint64_t> unmatched;
    std::vector<std::byte> converted(static_cast<std::size_t>(pixels));
    diagnostics.totalPixels = pixels;
    bool rejected = false;
    for (std::size_t pixel = 0U; pixel < static_cast<std::size_t>(pixels); ++pixel) {
        const std::size_t offset = pixel * 4U;
        SpriteColor8 source{
            std::to_integer<std::uint8_t>(rgba8[offset]),
            std::to_integer<std::uint8_t>(rgba8[offset + 1U]),
            std::to_integer<std::uint8_t>(rgba8[offset + 2U]),
            std::to_integer<std::uint8_t>(rgba8[offset + 3U]),
        };
        if (palette.transparentIndex && source.a <= settings.alphaThreshold) {
            converted[pixel] = static_cast<std::byte>(*palette.transparentIndex);
            ++diagnostics.transparentPixels;
            continue;
        }
        const auto exact = std::find(colors.begin(), colors.end(), source);
        if (exact != colors.end()) {
            converted[pixel] = static_cast<std::byte>(
                static_cast<std::uint8_t>(std::distance(colors.begin(), exact)));
            ++diagnostics.exactPixels;
            continue;
        }

        ++diagnostics.unmatchedPixels;
        const std::array<std::uint8_t, 4> key{source.r, source.g, source.b, source.a};
        ++unmatched[key];
        if (settings.policy == SpritePaletteIndexPolicy::Nearest) {
            std::uint32_t bestDistance = std::numeric_limits<std::uint32_t>::max();
            std::size_t bestIndex{};
            for (std::size_t index = 0U; index < colors.size(); ++index) {
                const std::uint32_t distance = color_distance_squared(source, colors[index]);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    bestIndex = index;
                }
            }
            converted[pixel] = static_cast<std::byte>(static_cast<std::uint8_t>(bestIndex));
            ++diagnostics.nearestPixels;
            diagnostics.maximumDistanceSquared = std::max(
                diagnostics.maximumDistanceSquared, bestDistance);
        } else {
            converted[pixel] = static_cast<std::byte>(
                static_cast<std::uint8_t>(settings.unmatchedIndex));
            if (settings.policy == SpritePaletteIndexPolicy::Error) rejected = true;
        }
    }
    for (const auto& [key, count] : unmatched) {
        if (diagnostics.unmatchedColors.size() >= kMaximumDiagnosticColors) break;
        diagnostics.unmatchedColors.push_back({{key[0], key[1], key[2], key[3]}, count});
    }
    if (rejected)
        return fail(error, "sprite palette conversion rejected unmatched source colors");
    outIndices = std::move(converted);
    return true;
}

bool encode_sprite_indices_rgba8(
    std::span<const std::byte> indices,
    const SpritePaletteAsset& palette,
    std::vector<std::byte>& outRgba8,
    std::string* error) {
    outRgba8.clear();
    if (!palette.validate(error)) return false;
    if (indices.size() > std::numeric_limits<std::size_t>::max() / 4U)
        return fail(error, "sprite index image is too large");
    outRgba8.resize(indices.size() * 4U);
    for (std::size_t pixel = 0U; pixel < indices.size(); ++pixel) {
        const std::uint8_t index = std::to_integer<std::uint8_t>(indices[pixel]);
        if (index >= palette.entry_count()) {
            outRgba8.clear();
            return fail(error, "sprite index image references a palette entry out of range");
        }
        const std::size_t offset = pixel * 4U;
        outRgba8[offset] = static_cast<std::byte>(index);
        outRgba8[offset + 1U] = std::byte{0};
        outRgba8[offset + 2U] = std::byte{0};
        outRgba8[offset + 3U] = static_cast<std::byte>(
            palette.transparentIndex && index == *palette.transparentIndex ? 0U : 255U);
    }
    return true;
}

} // namespace dve
