#include "dve/brick_palette.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <type_traits>
#include <map>

namespace dve {
namespace {

constexpr std::uint64_t kHashSeed = 1469598103934665603ULL;

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= 1099511628211ULL;
}

template<class Integer>
void hash_integer(std::uint64_t& hash, Integer value) noexcept {
    using Unsigned = std::make_unsigned_t<Integer>;
    Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t shift = 0U; shift < sizeof(Unsigned) * 8U; shift += 8U)
        hash_byte(hash, static_cast<std::uint8_t>((bits >> shift) & static_cast<Unsigned>(0xFFU)));
}

void hash_string(std::uint64_t& hash, std::string_view value) noexcept {
    hash_integer(hash, value.size());
    for (const char character : value) hash_byte(hash, static_cast<std::uint8_t>(character));
}

void hash_float(std::uint64_t& hash, float value) noexcept {
    std::uint32_t bits{};
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    hash_integer(hash, bits);
}

struct RankedMaterial {
    std::uint32_t globalMaterialId{};
    double totalWeight{};
};

[[nodiscard]] double deterministic_sum(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    long double total = 0.0L;
    for (const double value : values) total += static_cast<long double>(value);
    return static_cast<double>(total);
}

[[nodiscard]] BrickSourceSample canonicalize_sample(
    const BrickSourceSample& source, float epsilon) {
    std::map<std::uint32_t, std::vector<double>> grouped;
    for (const auto& contribution : source.contributions) {
        if (!std::isfinite(contribution.weight) || contribution.weight <= epsilon) continue;
        grouped[contribution.globalMaterialId].push_back(static_cast<double>(contribution.weight));
    }

    BrickSourceSample canonical;
    canonical.contributions.reserve(grouped.size());
    for (auto& [materialId, weights] : grouped) {
        const double weight = deterministic_sum(std::move(weights));
        if (weight <= static_cast<double>(epsilon)) continue;
        canonical.contributions.push_back({materialId, static_cast<float>(weight)});
    }
    return canonical;
}

[[nodiscard]] std::uint8_t clamp_slots(std::uint8_t requested) noexcept {
    if (requested == 0U) return 1U;
    if (requested > kBrickPaletteMaximumSlots) return kBrickPaletteMaximumSlots;
    return requested;
}

[[nodiscard]] BrickPaletteEncoding encoding_for_slot_count(std::size_t count) noexcept {
    if (count == 0U) return BrickPaletteEncoding::Empty;
    if (count == 1U) return BrickPaletteEncoding::Single;
    if (count == 2U) return BrickPaletteEncoding::Palette2;
    return BrickPaletteEncoding::Palette4;
}

[[nodiscard]] VoxelMaterialRuntimePath path_for_encoding(BrickPaletteEncoding encoding) noexcept {
    switch (encoding) {
        case BrickPaletteEncoding::Empty: return VoxelMaterialRuntimePath::NoPath;
        case BrickPaletteEncoding::Single: return VoxelMaterialRuntimePath::SingleMaterial;
        case BrickPaletteEncoding::Palette2: return VoxelMaterialRuntimePath::DeferredPalette2;
        case BrickPaletteEncoding::Palette4: return VoxelMaterialRuntimePath::DeferredPalette4;
    }
    return VoxelMaterialRuntimePath::NoPath;
}

// Largest remainder quantization onto a fixed 255 denominator. Deterministic: ties resolve toward
// the lower entry index so that identical source data always produces identical bytes.
void quantize_weights(const std::vector<float>& normalized, std::vector<std::uint8_t>& out) {
    out.assign(normalized.size(), 0U);
    if (normalized.empty()) return;

    std::vector<float> fractions(normalized.size(), 0.0F);
    std::uint32_t total = 0U;
    for (std::size_t index = 0U; index < normalized.size(); ++index) {
        const float raw = normalized[index] * static_cast<float>(kBrickPaletteWeightDenominator);
        float floored = std::floor(raw);
        if (!(floored >= 0.0F)) floored = 0.0F;
        if (floored > static_cast<float>(kBrickPaletteWeightDenominator))
            floored = static_cast<float>(kBrickPaletteWeightDenominator);
        out[index] = static_cast<std::uint8_t>(floored);
        fractions[index] = raw - floored;
        total += out[index];
    }

    std::vector<std::size_t> order(normalized.size(), 0U);
    for (std::size_t index = 0U; index < order.size(); ++index) order[index] = index;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        if (fractions[left] != fractions[right]) return fractions[left] > fractions[right];
        return left < right;
    });

    std::size_t cursor = 0U;
    while (total < kBrickPaletteWeightDenominator) {
        const std::size_t index = order[cursor % order.size()];
        if (out[index] < kBrickPaletteWeightDenominator) {
            out[index] = static_cast<std::uint8_t>(out[index] + 1U);
            ++total;
        }
        ++cursor;
        if (cursor > order.size() * (kBrickPaletteWeightDenominator + 1U)) break;
    }
    std::size_t reverse = 0U;
    while (total > kBrickPaletteWeightDenominator) {
        const std::size_t index = order[order.size() - 1U - (reverse % order.size())];
        if (out[index] > 0U) {
            out[index] = static_cast<std::uint8_t>(out[index] - 1U);
            --total;
        }
        ++reverse;
        if (reverse > order.size() * (kBrickPaletteWeightDenominator + 1U)) break;
    }
}

void write_u8(std::vector<std::uint8_t>& bytes, std::uint8_t value) { bytes.push_back(value); }

void write_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
}

void write_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U)
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
}

void write_f32(std::vector<std::uint8_t>& bytes, float value) {
    std::uint32_t bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    write_u32(bytes, bits);
}

void write_string(std::vector<std::uint8_t>& bytes, const std::string& value) {
    write_u32(bytes, static_cast<std::uint32_t>(value.size()));
    for (const char character : value) bytes.push_back(static_cast<std::uint8_t>(character));
}

struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t cursor{};
    bool failed{};

    [[nodiscard]] bool has(std::size_t count) const noexcept {
        return !failed && cursor + count <= bytes.size();
    }
    std::uint8_t u8() {
        if (!has(1U)) { failed = true; return 0U; }
        return bytes[cursor++];
    }
    std::uint32_t u32() {
        if (!has(4U)) { failed = true; return 0U; }
        std::uint32_t value = 0U;
        for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
            value |= static_cast<std::uint32_t>(bytes[cursor++]) << shift;
        return value;
    }
    std::uint64_t u64() {
        if (!has(8U)) { failed = true; return 0U; }
        std::uint64_t value = 0U;
        for (std::uint32_t shift = 0U; shift < 64U; shift += 8U)
            value |= static_cast<std::uint64_t>(bytes[cursor++]) << shift;
        return value;
    }
    float f32() {
        const std::uint32_t bits = u32();
        float value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    std::string text(std::uint32_t maximum) {
        const std::uint32_t length = u32();
        if (length > maximum || !has(length)) { failed = true; return {}; }
        std::string value;
        value.reserve(length);
        for (std::uint32_t index = 0U; index < length; ++index)
            value.push_back(static_cast<char>(bytes[cursor++]));
        return value;
    }
};

void escape_json(std::ostringstream& stream, std::string_view value) {
    for (const char character : value) {
        switch (character) {
            case '"': stream << "\\\""; break;
            case '\\': stream << "\\\\"; break;
            case '\n': stream << "\\n"; break;
            case '\r': stream << "\\r"; break;
            case '\t': stream << "\\t"; break;
            default: stream << character; break;
        }
    }
}


[[nodiscard]] std::uint64_t brick_palette_content_hash_v1(
    const CookedBrickPalette& palette) noexcept {
    std::uint64_t hash = kHashSeed;
    hash_integer(hash, palette.formatVersion);
    hash_integer(hash, palette.brickId);
    hash_string(hash, palette.assetName);
    hash_integer(hash, static_cast<std::uint8_t>(palette.encoding));
    hash_integer(hash, static_cast<std::uint8_t>(palette.runtimePath));
    hash_integer(hash, static_cast<std::uint32_t>(palette.slots.size()));
    for (const std::uint32_t slot : palette.slots) hash_integer(hash, slot);
    hash_integer(hash, static_cast<std::uint32_t>(palette.canonicalMaterials.size()));
    for (const std::uint32_t material : palette.canonicalMaterials) hash_integer(hash, material);
    hash_integer(hash, static_cast<std::uint32_t>(palette.samples.size()));
    for (const auto& sample : palette.samples) {
        hash_integer(hash, sample.usedSlots);
        for (std::uint8_t index = 0U; index < kBrickPaletteMaximumSlots; ++index) {
            hash_integer(hash, sample.slotIndices[index]);
            hash_integer(hash, sample.quantizedWeights[index]);
        }
    }
    hash_integer(hash, palette.sourceMaterialCount);
    hash_integer(hash, palette.overflowSampleCount);
    hash_integer(hash, palette.degenerateSampleCount);
    hash_float(hash, palette.maximumWeightError);
    hash_float(hash, palette.maximumReductionError);
    hash_byte(hash, palette.overflowed ? 1U : 0U);
    hash_byte(hash, palette.recookRequested ? 1U : 0U);
    hash_byte(hash, palette.valid ? 1U : 0U);
    hash_string(hash, palette.reason);
    return hash;
}

} // namespace

const char* to_string(BrickPaletteEncoding value) noexcept {
    switch (value) {
        case BrickPaletteEncoding::Empty: return "empty";
        case BrickPaletteEncoding::Single: return "single";
        case BrickPaletteEncoding::Palette2: return "palette2";
        case BrickPaletteEncoding::Palette4: return "palette4";
    }
    return "unknown";
}

float brick_palette_sample_weight(
    const BrickPaletteSampleEncoding& sample, std::uint8_t encodedIndex) noexcept {
    if (encodedIndex >= sample.usedSlots || encodedIndex >= kBrickPaletteMaximumSlots) return 0.0F;
    return static_cast<float>(sample.quantizedWeights[encodedIndex]) /
           static_cast<float>(kBrickPaletteWeightDenominator);
}

std::uint64_t brick_palette_content_hash(const CookedBrickPalette& palette) noexcept {
    std::uint64_t hash = kHashSeed;
    hash_integer(hash, palette.formatVersion);
    hash_integer(hash, palette.brickId);
    hash_string(hash, palette.assetName);
    hash_integer(hash, static_cast<std::uint8_t>(palette.encoding));
    hash_integer(hash, static_cast<std::uint8_t>(palette.runtimePath));
    hash_integer(hash, static_cast<std::uint32_t>(palette.slots.size()));
    for (const std::uint32_t slot : palette.slots) hash_integer(hash, slot);
    hash_integer(hash, static_cast<std::uint32_t>(palette.canonicalMaterials.size()));
    for (const std::uint32_t material : palette.canonicalMaterials) hash_integer(hash, material);
    hash_integer(hash, static_cast<std::uint32_t>(palette.canonicalSamples.size()));
    for (const auto& sample : palette.canonicalSamples) {
        hash_integer(hash, static_cast<std::uint32_t>(sample.contributions.size()));
        for (const auto& contribution : sample.contributions) {
            hash_integer(hash, contribution.globalMaterialId);
            hash_float(hash, contribution.weight);
        }
    }
    hash_integer(hash, static_cast<std::uint32_t>(palette.samples.size()));
    for (const auto& sample : palette.samples) {
        hash_integer(hash, sample.usedSlots);
        for (std::uint8_t index = 0U; index < kBrickPaletteMaximumSlots; ++index) {
            hash_integer(hash, sample.slotIndices[index]);
            hash_integer(hash, sample.quantizedWeights[index]);
        }
    }
    hash_integer(hash, palette.sourceMaterialCount);
    hash_integer(hash, palette.overflowSampleCount);
    hash_integer(hash, palette.degenerateSampleCount);
    hash_float(hash, palette.maximumWeightError);
    hash_float(hash, palette.maximumReductionError);
    hash_byte(hash, palette.overflowed ? 1U : 0U);
    hash_byte(hash, palette.recookRequested ? 1U : 0U);
    hash_byte(hash, palette.valid ? 1U : 0U);
    hash_string(hash, palette.reason);
    return hash;
}

CookedBrickPalette cook_brick_palette(
    const BrickSourceSurface& surface, const BrickPaletteCookConfig& config) {
    CookedBrickPalette palette;
    palette.brickId = surface.brickId;
    palette.assetName = surface.assetName;

    const std::uint8_t maximumSlots = clamp_slots(config.maximumPaletteSlots);
    const float epsilon = config.weightEpsilon > 0.0F ? config.weightEpsilon : 0.0F;

    std::vector<BrickSourceSample> canonicalSamples;
    canonicalSamples.reserve(surface.samples.size());
    for (const auto& sample : surface.samples)
        canonicalSamples.push_back(canonicalize_sample(sample, epsilon));

    std::map<std::uint32_t, std::vector<double>> totalContributions;
    for (const auto& sample : canonicalSamples) {
        for (const auto& contribution : sample.contributions)
            totalContributions[contribution.globalMaterialId].push_back(
                static_cast<double>(contribution.weight));
    }

    std::vector<RankedMaterial> ranked;
    ranked.reserve(totalContributions.size());
    for (auto& [materialId, weights] : totalContributions)
        ranked.push_back({materialId, deterministic_sum(std::move(weights))});
    std::sort(ranked.begin(), ranked.end(), [](const RankedMaterial& left, const RankedMaterial& right) {
        if (left.totalWeight != right.totalWeight) return left.totalWeight > right.totalWeight;
        return left.globalMaterialId < right.globalMaterialId;
    });

    palette.sourceMaterialCount = static_cast<std::uint32_t>(ranked.size());
    palette.canonicalMaterials.reserve(ranked.size());
    for (const auto& entry : ranked) palette.canonicalMaterials.push_back(entry.globalMaterialId);
    std::sort(palette.canonicalMaterials.begin(), palette.canonicalMaterials.end());
    if (config.preserveCanonicalSource) palette.canonicalSamples = canonicalSamples;

    if (ranked.empty()) {
        palette.encoding = BrickPaletteEncoding::Empty;
        palette.runtimePath = VoxelMaterialRuntimePath::NoPath;
        palette.valid = true;
        palette.reason = "no material contributions above the weight epsilon";
        palette.contentHash = brick_palette_content_hash(palette);
        return palette;
    }

    palette.overflowed = ranked.size() > static_cast<std::size_t>(maximumSlots);
    if (palette.overflowed) {
        switch (config.overflowPolicy) {
            case VoxelMaterialPaletteOverflowPolicy::BakeProperties:
                palette.encoding = BrickPaletteEncoding::Empty;
                palette.runtimePath = VoxelMaterialRuntimePath::BakedProperties;
                palette.valid = true;
                palette.reason = "palette overflow baked to conventional properties";
                palette.contentHash = brick_palette_content_hash(palette);
                return palette;
            case VoxelMaterialPaletteOverflowPolicy::RequestRecook:
                palette.encoding = BrickPaletteEncoding::Empty;
                palette.runtimePath = VoxelMaterialRuntimePath::NoPath;
                palette.recookRequested = true;
                palette.valid = true;
                palette.reason = "palette overflow deferred to asynchronous recook";
                palette.contentHash = brick_palette_content_hash(palette);
                return palette;
            case VoxelMaterialPaletteOverflowPolicy::Reject:
                palette.encoding = BrickPaletteEncoding::Empty;
                palette.runtimePath = VoxelMaterialRuntimePath::NoPath;
                palette.valid = false;
                palette.reason = "palette overflow rejected by cook policy";
                palette.contentHash = brick_palette_content_hash(palette);
                return palette;
            case VoxelMaterialPaletteOverflowPolicy::DominantMaterial:
                break;
        }
    }

    const std::size_t slotCount = std::min<std::size_t>(ranked.size(), maximumSlots);
    palette.slots.reserve(slotCount);
    for (std::size_t index = 0U; index < slotCount; ++index)
        palette.slots.push_back(ranked[index].globalMaterialId);

    palette.encoding = encoding_for_slot_count(slotCount);
    palette.runtimePath = path_for_encoding(palette.encoding);

    std::map<std::uint32_t, std::uint8_t> slotOf;
    for (std::size_t index = 0U; index < palette.slots.size(); ++index)
        slotOf[palette.slots[index]] = static_cast<std::uint8_t>(index);

    palette.samples.reserve(canonicalSamples.size());
    std::vector<float> kept;
    std::vector<std::uint8_t> keptSlots;
    std::vector<float> normalized;
    std::vector<std::uint8_t> quantized;

    for (const auto& sample : canonicalSamples) {
        std::array<double, kBrickPaletteMaximumSlots> perSlot{};
        double totalMass = 0.0;
        double keptMass = 0.0;
        for (const auto& contribution : sample.contributions) {
            const double weight = static_cast<double>(contribution.weight);
            totalMass += weight;
            const auto found = slotOf.find(contribution.globalMaterialId);
            if (found == slotOf.end()) continue;
            perSlot[found->second] += weight;
            keptMass += weight;
        }

        const double droppedMass = totalMass - keptMass;
        if (totalMass > 0.0 && droppedMass > static_cast<double>(epsilon)) {
            ++palette.overflowSampleCount;
            const float reductionError = static_cast<float>(droppedMass / totalMass);
            palette.maximumReductionError = std::max(palette.maximumReductionError, reductionError);
        }

        BrickPaletteSampleEncoding encoded;
        if (keptMass <= 0.0) {
            ++palette.degenerateSampleCount;
            encoded.usedSlots = 1U;
            encoded.slotIndices[0] = 0U;
            encoded.quantizedWeights[0] = kBrickPaletteWeightDenominator;
            palette.samples.push_back(encoded);
            continue;
        }

        kept.clear();
        keptSlots.clear();
        for (std::uint8_t index = 0U; index < static_cast<std::uint8_t>(palette.slots.size()); ++index) {
            if (perSlot[index] <= 0.0) continue;
            kept.push_back(static_cast<float>(perSlot[index]));
            keptSlots.push_back(index);
        }

        normalized.assign(kept.size(), 0.0F);
        for (std::size_t index = 0U; index < kept.size(); ++index)
            normalized[index] = static_cast<float>(static_cast<double>(kept[index]) / keptMass);
        quantize_weights(normalized, quantized);

        std::uint8_t used = 0U;
        for (std::size_t index = 0U; index < quantized.size(); ++index) {
            const float error = std::fabs(
                static_cast<float>(quantized[index]) /
                    static_cast<float>(kBrickPaletteWeightDenominator) - normalized[index]);
            palette.maximumWeightError = std::max(palette.maximumWeightError, error);
            if (quantized[index] == 0U) continue;
            encoded.slotIndices[used] = keptSlots[index];
            encoded.quantizedWeights[used] = quantized[index];
            ++used;
        }
        encoded.usedSlots = used;
        palette.samples.push_back(encoded);
    }

    palette.valid = true;
    palette.reason = palette.overflowed ? "reduced to dominant palette slots" : "palette cooked";
    palette.contentHash = brick_palette_content_hash(palette);
    return palette;
}

BrickPaletteCookReport build_brick_palette_cook_report(
    std::span<const BrickSourceSurface> surfaces, const BrickPaletteCookConfig& config) {
    BrickPaletteCookReport report;
    report.bricks.reserve(surfaces.size());
    for (const auto& surface : surfaces) {
        CookedBrickPalette palette = cook_brick_palette(surface, config);
        ++report.brickCount;
        if (!palette.valid) ++report.rejectedBrickCount;
        if (palette.recookRequested) ++report.recookRequestCount;
        if (palette.overflowed) ++report.overflowBrickCount;
        if (palette.runtimePath == VoxelMaterialRuntimePath::BakedProperties) ++report.bakedBrickCount;
        switch (palette.encoding) {
            case BrickPaletteEncoding::Empty:
                if (palette.valid && palette.runtimePath == VoxelMaterialRuntimePath::NoPath &&
                    !palette.recookRequested)
                    ++report.emptyBrickCount;
                break;
            case BrickPaletteEncoding::Single: ++report.singleMaterialBrickCount; break;
            case BrickPaletteEncoding::Palette2: ++report.palette2BrickCount; break;
            case BrickPaletteEncoding::Palette4: ++report.palette4BrickCount; break;
        }
        report.sampleCount += palette.samples.size();
        report.paletteSlotCount += palette.slots.size();
        report.payloadBytes += serialize_brick_palette(palette).size();
        report.maximumWeightError = std::max(report.maximumWeightError, palette.maximumWeightError);
        report.maximumReductionError =
            std::max(report.maximumReductionError, palette.maximumReductionError);
        report.bricks.push_back(std::move(palette));
    }

    std::uint64_t hash = kHashSeed;
    for (const auto value : {report.brickCount, report.emptyBrickCount,
                             report.singleMaterialBrickCount, report.palette2BrickCount,
                             report.palette4BrickCount, report.bakedBrickCount,
                             report.rejectedBrickCount, report.recookRequestCount,
                             report.overflowBrickCount, report.sampleCount,
                             report.paletteSlotCount, report.payloadBytes})
        hash_integer(hash, value);
    hash_float(hash, report.maximumWeightError);
    hash_float(hash, report.maximumReductionError);
    for (const auto& brick : report.bricks) hash_integer(hash, brick.contentHash);
    report.contentHash = hash;
    return report;
}

BrickPaletteRemapTable build_brick_palette_remap(std::span<const CookedBrickPalette> bricks) {
    BrickPaletteRemapTable table;
    for (const auto& brick : bricks)
        for (const std::uint32_t slot : brick.slots) table.globalMaterialIds.push_back(slot);
    std::sort(table.globalMaterialIds.begin(), table.globalMaterialIds.end());
    table.globalMaterialIds.erase(
        std::unique(table.globalMaterialIds.begin(), table.globalMaterialIds.end()),
        table.globalMaterialIds.end());
    return table;
}

std::vector<std::uint32_t> remap_brick_palette_slots(
    const CookedBrickPalette& brick, const BrickPaletteRemapTable& table) {
    std::vector<std::uint32_t> mapping;
    mapping.reserve(brick.slots.size());
    for (const std::uint32_t slot : brick.slots) {
        const auto found = std::lower_bound(
            table.globalMaterialIds.begin(), table.globalMaterialIds.end(), slot);
        if (found == table.globalMaterialIds.end() || *found != slot) return {};
        mapping.push_back(static_cast<std::uint32_t>(
            std::distance(table.globalMaterialIds.begin(), found)));
    }
    return mapping;
}

std::vector<std::uint8_t> serialize_brick_palette(
    const CookedBrickPalette& palette, std::uint32_t formatVersion) {
    if (formatVersion > kBrickPaletteFormatVersion)
        throw std::invalid_argument("unsupported brick palette serialization version");

    std::vector<std::uint8_t> bytes;
    for (const char character : kBrickPaletteMagic)
        bytes.push_back(static_cast<std::uint8_t>(character));
    bytes.push_back(0U);
    write_u32(bytes, formatVersion);
    write_u64(bytes, palette.brickId);
    write_string(bytes, palette.assetName);
    write_u8(bytes, static_cast<std::uint8_t>(palette.encoding));

    write_u32(bytes, static_cast<std::uint32_t>(palette.slots.size()));
    for (const std::uint32_t slot : palette.slots) write_u32(bytes, slot);

    if (formatVersion >= 1U) {
        write_u8(bytes, static_cast<std::uint8_t>(palette.runtimePath));
        write_u8(bytes, palette.overflowed ? 1U : 0U);
        write_u8(bytes, palette.recookRequested ? 1U : 0U);
        write_u8(bytes, palette.valid ? 1U : 0U);
        write_string(bytes, palette.reason);
        write_u32(bytes, palette.sourceMaterialCount);
        write_u32(bytes, palette.overflowSampleCount);
        write_u32(bytes, palette.degenerateSampleCount);
        write_f32(bytes, palette.maximumWeightError);
        write_f32(bytes, palette.maximumReductionError);
        write_u32(bytes, static_cast<std::uint32_t>(palette.canonicalMaterials.size()));
        for (const std::uint32_t material : palette.canonicalMaterials) write_u32(bytes, material);
    }
    if (formatVersion >= 2U) {
        write_u32(bytes, static_cast<std::uint32_t>(palette.canonicalSamples.size()));
        for (const auto& sample : palette.canonicalSamples) {
            write_u32(bytes, static_cast<std::uint32_t>(sample.contributions.size()));
            for (const auto& contribution : sample.contributions) {
                write_u32(bytes, contribution.globalMaterialId);
                write_f32(bytes, contribution.weight);
            }
        }
    }

    write_u32(bytes, static_cast<std::uint32_t>(palette.samples.size()));
    for (const auto& sample : palette.samples) {
        write_u8(bytes, sample.usedSlots);
        for (std::uint8_t index = 0U; index < kBrickPaletteMaximumSlots; ++index)
            write_u8(bytes, sample.slotIndices[index]);
        if (formatVersion >= 1U) {
            for (std::uint8_t index = 0U; index < kBrickPaletteMaximumSlots; ++index)
                write_u8(bytes, sample.quantizedWeights[index]);
        }
    }

    CookedBrickPalette hashed = palette;
    hashed.formatVersion = formatVersion;
    if (formatVersion < 2U) hashed.canonicalSamples.clear();
    const std::uint64_t storedHash = formatVersion >= 2U
        ? brick_palette_content_hash(hashed)
        : brick_palette_content_hash_v1(hashed);
    write_u64(bytes, storedHash);
    return bytes;
}

bool deserialize_brick_palette(
    std::span<const std::uint8_t> bytes, CookedBrickPalette& out, std::string* error) {
    const auto fail = [&](const char* message) {
        if (error != nullptr) *error = message;
        return false;
    };

    Reader reader{bytes, 0U, false};
    if (bytes.size() < kBrickPaletteMagic.size() + 1U) return fail("payload shorter than header");
    for (const char character : kBrickPaletteMagic)
        if (reader.u8() != static_cast<std::uint8_t>(character)) return fail("bad magic");
    if (reader.u8() != 0U) return fail("bad magic terminator");

    CookedBrickPalette palette;
    const std::uint32_t version = reader.u32();
    if (version < kBrickPaletteMinimumSupportedVersion || version > kBrickPaletteFormatVersion)
        return fail("unsupported brick palette format version");
    palette.sourceFormatVersion = version;
    palette.formatVersion = version;
    palette.brickId = reader.u64();
    palette.assetName = reader.text(4096U);
    const std::uint8_t encoding = reader.u8();
    if (encoding > static_cast<std::uint8_t>(BrickPaletteEncoding::Palette4))
        return fail("unknown encoding");
    palette.encoding = static_cast<BrickPaletteEncoding>(encoding);

    const std::uint32_t slotCount = reader.u32();
    if (reader.failed) return fail("truncated payload");
    if (slotCount > kBrickPaletteMaximumSlots) return fail("slot count exceeds maximum");
    palette.slots.reserve(slotCount);
    for (std::uint32_t index = 0U; index < slotCount; ++index)
        palette.slots.push_back(reader.u32());

    if (version >= 1U) {
        const std::uint8_t path = reader.u8();
        if (path > static_cast<std::uint8_t>(VoxelMaterialRuntimePath::DeferredPalette4))
            return fail("unknown runtime path");
        palette.runtimePath = static_cast<VoxelMaterialRuntimePath>(path);
        palette.overflowed = reader.u8() != 0U;
        palette.recookRequested = reader.u8() != 0U;
        palette.valid = reader.u8() != 0U;
        palette.reason = reader.text(4096U);
        palette.sourceMaterialCount = reader.u32();
        palette.overflowSampleCount = reader.u32();
        palette.degenerateSampleCount = reader.u32();
        palette.maximumWeightError = reader.f32();
        palette.maximumReductionError = reader.f32();
        const std::uint32_t canonicalCount = reader.u32();
        if (reader.failed) return fail("truncated payload");
        if (canonicalCount > (1U << 20U)) return fail("canonical material count out of range");
        palette.canonicalMaterials.reserve(canonicalCount);
        for (std::uint32_t index = 0U; index < canonicalCount; ++index)
            palette.canonicalMaterials.push_back(reader.u32());
    } else {
        palette.runtimePath = path_for_encoding(palette.encoding);
        palette.valid = true;
        palette.sourceMaterialCount = slotCount;
        palette.canonicalMaterials = palette.slots;
        std::sort(palette.canonicalMaterials.begin(), palette.canonicalMaterials.end());
        palette.reason = "migrated from brick palette format version 0";
    }

    if (version >= 2U) {
        const std::uint32_t canonicalSampleCount = reader.u32();
        if (reader.failed) return fail("truncated canonical source");
        if (canonicalSampleCount > (1U << 24U)) return fail("canonical sample count out of range");
        palette.canonicalSamples.reserve(canonicalSampleCount);
        for (std::uint32_t sampleIndex = 0U; sampleIndex < canonicalSampleCount; ++sampleIndex) {
            BrickSourceSample sample;
            const std::uint32_t contributionCount = reader.u32();
            if (contributionCount > (1U << 20U)) return fail("canonical contribution count out of range");
            sample.contributions.reserve(contributionCount);
            for (std::uint32_t contributionIndex = 0U;
                 contributionIndex < contributionCount; ++contributionIndex) {
                BrickMaterialContribution contribution;
                contribution.globalMaterialId = reader.u32();
                contribution.weight = reader.f32();
                if (!std::isfinite(contribution.weight) || contribution.weight <= 0.0F)
                    return fail("invalid canonical contribution weight");
                sample.contributions.push_back(contribution);
            }
            palette.canonicalSamples.push_back(std::move(sample));
        }
    }

    const std::uint32_t sampleCount = reader.u32();
    if (reader.failed) return fail("truncated payload");
    if (sampleCount > (1U << 24U)) return fail("sample count out of range");
    palette.samples.reserve(sampleCount);
    for (std::uint32_t index = 0U; index < sampleCount; ++index) {
        BrickPaletteSampleEncoding sample;
        sample.usedSlots = reader.u8();
        if (sample.usedSlots > kBrickPaletteMaximumSlots) return fail("sample uses too many slots");
        for (std::uint8_t slot = 0U; slot < kBrickPaletteMaximumSlots; ++slot)
            sample.slotIndices[slot] = reader.u8();
        if (version >= 1U) {
            for (std::uint8_t slot = 0U; slot < kBrickPaletteMaximumSlots; ++slot)
                sample.quantizedWeights[slot] = reader.u8();
        } else if (sample.usedSlots > 0U) {
            std::vector<float> uniform(sample.usedSlots, 1.0F / static_cast<float>(sample.usedSlots));
            std::vector<std::uint8_t> quantized;
            quantize_weights(uniform, quantized);
            for (std::size_t slot = 0U; slot < quantized.size(); ++slot)
                sample.quantizedWeights[slot] = quantized[slot];
        }
        if (reader.failed) return fail("truncated payload");
        for (std::uint8_t slot = 0U; slot < sample.usedSlots; ++slot)
            if (sample.slotIndices[slot] >= slotCount) return fail("sample references missing slot");
        palette.samples.push_back(sample);
    }

    const std::uint64_t storedHash = reader.u64();
    if (reader.failed || reader.cursor != bytes.size()) return fail("truncated or trailing payload data");

    if (version >= 2U) {
        if (brick_palette_content_hash(palette) != storedHash) return fail("content hash mismatch");
    } else if (version >= 1U) {
        if (brick_palette_content_hash_v1(palette) != storedHash) return fail("content hash mismatch");
    }

    palette.formatVersion = kBrickPaletteFormatVersion;
    if (version < kBrickPaletteFormatVersion) {
        if (!palette.reason.empty()) palette.reason += "; ";
        palette.reason += "migrated to brick palette format version 2";
    }
    palette.contentHash = brick_palette_content_hash(palette);
    out = std::move(palette);
    return true;
}

std::string brick_palette_cook_json(const BrickPaletteCookReport& report) {
    std::ostringstream stream;
    stream << "{\n";
    stream << "  \"formatVersion\": " << kBrickPaletteFormatVersion << ",\n";
    stream << "  \"brickCount\": " << report.brickCount << ",\n";
    stream << "  \"emptyBrickCount\": " << report.emptyBrickCount << ",\n";
    stream << "  \"singleMaterialBrickCount\": " << report.singleMaterialBrickCount << ",\n";
    stream << "  \"palette2BrickCount\": " << report.palette2BrickCount << ",\n";
    stream << "  \"palette4BrickCount\": " << report.palette4BrickCount << ",\n";
    stream << "  \"bakedBrickCount\": " << report.bakedBrickCount << ",\n";
    stream << "  \"rejectedBrickCount\": " << report.rejectedBrickCount << ",\n";
    stream << "  \"recookRequestCount\": " << report.recookRequestCount << ",\n";
    stream << "  \"overflowBrickCount\": " << report.overflowBrickCount << ",\n";
    stream << "  \"sampleCount\": " << report.sampleCount << ",\n";
    stream << "  \"paletteSlotCount\": " << report.paletteSlotCount << ",\n";
    stream << "  \"payloadBytes\": " << report.payloadBytes << ",\n";
    stream << "  \"maximumWeightError\": " << report.maximumWeightError << ",\n";
    stream << "  \"maximumReductionError\": " << report.maximumReductionError << ",\n";
    stream << "  \"bricks\": [\n";
    for (std::size_t index = 0U; index < report.bricks.size(); ++index) {
        const auto& brick = report.bricks[index];
        stream << "    {\n";
        stream << "      \"brickId\": " << brick.brickId << ",\n";
        stream << "      \"assetName\": \"";
        escape_json(stream, brick.assetName);
        stream << "\",\n";
        stream << "      \"encoding\": \"" << to_string(brick.encoding) << "\",\n";
        stream << "      \"runtimePath\": \"" << to_string(brick.runtimePath) << "\",\n";
        stream << "      \"slots\": [";
        for (std::size_t slot = 0U; slot < brick.slots.size(); ++slot) {
            if (slot != 0U) stream << ", ";
            stream << brick.slots[slot];
        }
        stream << "],\n";
        stream << "      \"sampleCount\": " << brick.samples.size() << ",\n";
        stream << "      \"sourceMaterialCount\": " << brick.sourceMaterialCount << ",\n";
        stream << "      \"overflowSampleCount\": " << brick.overflowSampleCount << ",\n";
        stream << "      \"degenerateSampleCount\": " << brick.degenerateSampleCount << ",\n";
        stream << "      \"maximumWeightError\": " << brick.maximumWeightError << ",\n";
        stream << "      \"maximumReductionError\": " << brick.maximumReductionError << ",\n";
        stream << "      \"overflowed\": " << (brick.overflowed ? "true" : "false") << ",\n";
        stream << "      \"recookRequested\": " << (brick.recookRequested ? "true" : "false") << ",\n";
        stream << "      \"valid\": " << (brick.valid ? "true" : "false") << ",\n";
        stream << "      \"reason\": \"";
        escape_json(stream, brick.reason);
        stream << "\",\n";
        stream << "      \"contentHash\": " << brick.contentHash << "\n";
        stream << "    }" << (index + 1U < report.bricks.size() ? "," : "") << "\n";
    }
    stream << "  ],\n";
    stream << "  \"contentHash\": " << report.contentHash << "\n";
    stream << "}\n";
    return stream.str();
}

} // namespace dve
