#include "dve/brick_palette_asset_cooker.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <set>
#include <type_traits>

namespace dve {
namespace {

constexpr std::uint64_t kHashOffset = 1469598103934665603ULL;
constexpr std::uint64_t kHashPrime = 1099511628211ULL;

void hash_byte(std::uint64_t& hash, std::uint8_t value) noexcept {
    hash ^= value;
    hash *= kHashPrime;
}

template<class Integer>
void hash_integer(std::uint64_t& hash, Integer value) noexcept {
    using Unsigned = std::make_unsigned_t<Integer>;
    Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t shift = 0U; shift < sizeof(Unsigned) * 8U; shift += 8U)
        hash_byte(hash, static_cast<std::uint8_t>((bits >> shift) & static_cast<Unsigned>(0xFFU)));
}

void hash_float(std::uint64_t& hash, float value) noexcept {
    hash_integer(hash, std::bit_cast<std::uint32_t>(value));
}

void hash_string(std::uint64_t& hash, std::string_view value) noexcept {
    hash_integer(hash, value.size());
    for (const char character : value) hash_byte(hash, static_cast<std::uint8_t>(character));
}

void hash_retention(std::uint64_t& hash, const BrickPaletteRetentionProfile& profile) noexcept {
    hash_byte(hash, profile.retainBakedProperties ? 1U : 0U);
    hash_byte(hash, profile.retainSingleMaterial ? 1U : 0U);
    hash_byte(hash, profile.retainPalette2 ? 1U : 0U);
    hash_byte(hash, profile.retainPalette4 ? 1U : 0U);
    hash_byte(hash, profile.retainCanonicalSource ? 1U : 0U);
}

void hash_material(std::uint64_t& hash, const VoxelMaterialDefinition& material) noexcept {
    hash_string(hash, material.name);
    hash_float(hash, material.baseColor.x);
    hash_float(hash, material.baseColor.y);
    hash_float(hash, material.baseColor.z);
    hash_float(hash, material.baseColor.w);
    hash_float(hash, material.emissive.x);
    hash_float(hash, material.emissive.y);
    hash_float(hash, material.emissive.z);
    hash_float(hash, material.metallic);
    hash_float(hash, material.roughness);
    hash_float(hash, material.specular);
    hash_integer(hash, material.shadingModel);
    hash_integer(hash, material.blendMode);
    hash_float(hash, material.clearCoat);
    hash_float(hash, material.clearCoatRoughness);
    hash_byte(hash, material.transparent ? 1U : 0U);
}

[[nodiscard]] std::uint64_t brick_id(std::uint64_t objectId, BrickKey key) noexcept {
    std::uint64_t hash = kHashOffset;
    hash_string(hash, "DVE_BRICK_PALETTE_BRICK_1");
    hash_integer(hash, objectId);
    hash_integer(hash, key.x);
    hash_integer(hash, key.y);
    hash_integer(hash, key.z);
    return hash;
}

[[nodiscard]] BrickSourceSurface make_source_surface(
    const VoxelObject& object, BrickKey key, const Brick& brick) {
    BrickSourceSurface surface;
    surface.brickId = brick_id(object.id(), key);
    surface.assetName = "voxel-object-" + std::to_string(object.id()) + "/brick/" +
                        std::to_string(key.x) + "/" + std::to_string(key.y) + "/" +
                        std::to_string(key.z);
    surface.samples.resize(static_cast<std::size_t>(kBrickVoxelCount));
    for (std::uint16_t index = 0U; index < kBrickVoxelCount; ++index) {
        const MaterialId material = brick.material(index);
        if (material == kAirMaterial) continue;
        surface.samples[index].contributions.push_back(
            {static_cast<std::uint32_t>(material), 1.0F});
    }
    return surface;
}

[[nodiscard]] render::BrickPaletteSurfacePoint surface_point(
    BrickKey key, std::uint16_t index, float voxelSizeMeters) noexcept {
    const Int3 global = global_from_local(key, local_from_index_unchecked(index));
    render::BrickPaletteSurfacePoint point;
    point.worldPosition = {
        static_cast<float>(global.x) * voxelSizeMeters,
        static_cast<float>(global.y) * voxelSizeMeters,
        static_cast<float>(global.z) * voxelSizeMeters};
    point.worldNormal = {0.0F, 1.0F, 0.0F};
    point.assetU = static_cast<float>(global.x);
    point.assetV = static_cast<float>(global.z);
    return point;
}

[[nodiscard]] std::optional<CookedBrickPalette> cook_exact_palette(
    const BrickSourceSurface& source,
    std::uint8_t slots,
    VoxelMaterialPaletteOverflowPolicy overflow,
    bool preserveCanonical) {
    BrickPaletteCookConfig config;
    config.maximumPaletteSlots = slots;
    config.overflowPolicy = overflow;
    config.preserveCanonicalSource = preserveCanonical;
    CookedBrickPalette cooked = cook_brick_palette(source, config);
    const auto requested = slots == 2U ? VoxelMaterialRuntimePath::DeferredPalette2
                                      : VoxelMaterialRuntimePath::DeferredPalette4;
    if (!cooked.valid || cooked.runtimePath != requested) return std::nullopt;
    return cooked;
}

[[nodiscard]] CookedBrickMaterialRepresentations cook_one(
    const VoxelObject& object,
    BrickKey key,
    const Brick& brick,
    std::span<const render::BrickPaletteMaterialRecord> materialRecords,
    const BrickPaletteAssetCookSettings& settings) {
    CookedBrickMaterialRepresentations output;
    output.key = key;
    output.sourceGeneration = brick.generation();

    const BrickSourceSurface source = make_source_surface(object, key, brick);
    const bool needPalette4 = settings.retention.retainPalette4 || settings.retention.retainBakedProperties;
    std::optional<CookedBrickPalette> palette4;
    if (needPalette4) {
        palette4 = cook_exact_palette(source, 4U, settings.palette4Overflow,
                                      settings.retention.retainCanonicalSource);
    }

    if (settings.retention.retainPalette2) {
        output.representations.deferredPalette2 = cook_exact_palette(
            source, 2U, settings.palette2Overflow, settings.retention.retainCanonicalSource);
    }
    if (settings.retention.retainPalette4 && palette4.has_value())
        output.representations.deferredPalette4 = palette4;

    if (settings.retention.retainSingleMaterial) {
        std::vector<std::uint32_t> ids(static_cast<std::size_t>(kBrickVoxelCount), 0U);
        for (std::uint16_t index = 0U; index < kBrickVoxelCount; ++index)
            ids[index] = static_cast<std::uint32_t>(brick.material(index));
        output.representations.singleMaterialIds = std::move(ids);
    }

    if (settings.retention.retainBakedProperties) {
        std::vector<render::BrickPaletteShadedSample> baked(
            static_cast<std::size_t>(kBrickVoxelCount));
        if (palette4.has_value()) {
            for (std::uint16_t index = 0U; index < kBrickVoxelCount; ++index) {
                baked[index] = render::bake_brick_palette_sample(
                    *palette4, materialRecords, index,
                    surface_point(key, index, settings.voxelSizeMeters), settings.bakedShading);
            }
        } else {
            for (std::uint16_t index = 0U; index < kBrickVoxelCount; ++index) {
                const auto material = brick.material(index);
                if (material == kAirMaterial) continue;
                const auto* record = render::find_brick_palette_material(
                    materialRecords, static_cast<std::uint32_t>(material));
                if (record == nullptr) continue;
                auto& sample = baked[index];
                sample.baseColor = record->baseColor;
                sample.roughness = record->roughness;
                sample.metallic = record->metallic;
                sample.emissive = record->emissive;
                sample.opacity = record->opacity;
                sample.worldNormal = {0.0F, 1.0F, 0.0F};
                sample.slotsEvaluated = 1U;
                sample.textureSamples = 1U;
                sample.valid = true;
            }
        }
        output.representations.bakedProperties = std::move(baked);
    }

    if (settings.retention.retainCanonicalSource)
        output.representations.canonicalSource = source;

    output.representations.generation = brick.generation();

    std::uint64_t dependency = kHashOffset;
    hash_string(dependency, "DVE_BRICK_PALETTE_REPRESENTATIONS_1");
    hash_integer(dependency, object.id());
    hash_integer(dependency, key.x);
    hash_integer(dependency, key.y);
    hash_integer(dependency, key.z);
    hash_integer(dependency, VoxelObject::brick_content_hash(brick.materials()));
    hash_integer(dependency, brick.generation());
    hash_retention(dependency, settings.retention);
    hash_integer(dependency, settings.palette2Overflow);
    hash_integer(dependency, settings.palette4Overflow);
    hash_float(dependency, settings.voxelSizeMeters);
    for (const auto& material : materialRecords) {
        hash_integer(dependency, material.globalMaterialId);
        hash_float(dependency, material.baseColor.x);
        hash_float(dependency, material.baseColor.y);
        hash_float(dependency, material.baseColor.z);
        hash_float(dependency, material.roughness);
        hash_float(dependency, material.metallic);
        hash_float(dependency, material.opacity);
    }
    output.dependencyKey = dependency;
    return output;
}

void rebuild_asset_metadata(CookedBrickPaletteAsset& asset) {
    std::vector<CookedBrickPalette> palettes;
    asset.retainedBytes = 0U;
    asset.canonicalBytes = 0U;
    std::uint64_t hash = kHashOffset;
    hash_string(hash, "DVE_BRICK_PALETTE_ASSET_1");
    hash_integer(hash, asset.objectId);
    hash_integer(hash, asset.generation);
    hash_retention(hash, asset.retention);
    for (const auto& brick : asset.bricks) {
        hash_integer(hash, brick.key.x);
        hash_integer(hash, brick.key.y);
        hash_integer(hash, brick.key.z);
        hash_integer(hash, brick.sourceGeneration);
        hash_integer(hash, brick.dependencyKey);
        asset.retainedBytes += voxel_material_retained_bytes(brick.representations);
        if (brick.representations.canonicalSource.has_value()) {
            for (const auto& sample : brick.representations.canonicalSource->samples)
                asset.canonicalBytes += sample.contributions.size() * sizeof(BrickMaterialContribution);
        }
        if (brick.representations.deferredPalette4.has_value())
            palettes.push_back(*brick.representations.deferredPalette4);
        else if (brick.representations.deferredPalette2.has_value())
            palettes.push_back(*brick.representations.deferredPalette2);
    }
    asset.remap = build_brick_palette_remap(palettes);
    for (const auto material : asset.remap.globalMaterialIds) hash_integer(hash, material);
    hash_integer(hash, asset.retainedBytes);
    hash_integer(hash, asset.canonicalBytes);
    asset.contentHash = hash;
}

void set_error(std::string* error, std::string_view message) {
    if (error != nullptr) error->assign(message.begin(), message.end());
}

} // namespace


CookedVoxelAssetPalettePackage package_cooked_voxel_asset_brick_palettes(
    CookedVoxelAsset asset,
    const BrickPaletteAssetCookSettings& settings) {
    CookedVoxelAssetPalettePackage output;
    output.asset = std::move(asset);
    BrickPaletteAssetCookSettings resolved = settings;
    resolved.voxelSizeMeters = output.asset.voxelSizeMeters;
    output.brickPalettes = cook_voxel_object_brick_palettes(
        output.asset.object, output.asset.materials, resolved);
    output.success = std::none_of(output.asset.diagnostics.begin(), output.asset.diagnostics.end(),
        [](const ImportDiagnostic& diagnostic) {
            return diagnostic.severity == ImportDiagnostic::Severity::Error;
        });
    return output;
}

std::vector<render::BrickPaletteMaterialRecord> make_brick_palette_material_records(
    std::span<const VoxelMaterialDefinition> materials) {
    std::vector<render::BrickPaletteMaterialRecord> records;
    records.reserve(materials.size());
    for (std::size_t index = 0U; index < materials.size(); ++index) {
        const auto& material = materials[index];
        render::BrickPaletteMaterialRecord record;
        record.globalMaterialId = static_cast<std::uint32_t>(index);
        record.baseColor = {material.baseColor.x, material.baseColor.y, material.baseColor.z};
        record.roughness = material.roughness;
        record.metallic = material.metallic;
        record.emissive = {material.emissive.x, material.emissive.y, material.emissive.z};
        record.opacity = material.baseColor.w;
        records.push_back(record);
    }
    return records;
}

std::uint64_t brick_palette_asset_dependency_key(
    const VoxelObject& object,
    std::span<const VoxelMaterialDefinition> materials,
    const BrickPaletteAssetCookSettings& settings) noexcept {
    std::uint64_t hash = kHashOffset;
    hash_string(hash, "DVE_BRICK_PALETTE_ASSET_DEPENDENCY_1");
    hash_integer(hash, object.id());
    hash_integer(hash, object.state_hash());
    hash_retention(hash, settings.retention);
    hash_integer(hash, settings.palette2Overflow);
    hash_integer(hash, settings.palette4Overflow);
    hash_integer(hash, settings.bakedShading.mapping);
    hash_byte(hash, settings.bakedShading.blendNormalsAsVectors ? 1U : 0U);
    hash_byte(hash, settings.bakedShading.blendBaseColorInLinearSpace ? 1U : 0U);
    hash_float(hash, settings.voxelSizeMeters);
    for (const auto& material : materials) hash_material(hash, material);
    return hash;
}

CookedBrickPaletteAsset cook_voxel_object_brick_palettes(
    const VoxelObject& object,
    std::span<const VoxelMaterialDefinition> materials,
    const BrickPaletteAssetCookSettings& settings) {
    CookedBrickPaletteAsset output;
    output.objectId = object.id();
    output.dependencyKey = brick_palette_asset_dependency_key(object, materials, settings);
    output.generation = 1U;
    output.retention = settings.retention;
    const auto materialRecords = make_brick_palette_material_records(materials);
    for (const BrickKey key : object.bricks().sorted_keys()) {
        const Brick* brick = object.find_brick(key);
        if (brick == nullptr || brick->empty()) continue;
        output.bricks.push_back(cook_one(object, key, *brick, materialRecords, settings));
    }
    rebuild_asset_metadata(output);
    return output;
}

BrickPaletteAssetRecookResult recook_voxel_object_brick_palettes(
    CookedBrickPaletteAsset& asset,
    const VoxelObject& object,
    std::span<const VoxelMaterialDefinition> materials,
    std::span<const AppliedBrickEdit> edits,
    const BrickPaletteAssetCookSettings& settings) {
    BrickPaletteAssetRecookResult result;
    result.submittedEdits = edits.size();
    if (asset.objectId != object.id()) {
        result.reason = "recook object ID does not match the cooked asset";
        return result;
    }
    if (!(asset.retention.retainBakedProperties == settings.retention.retainBakedProperties &&
          asset.retention.retainSingleMaterial == settings.retention.retainSingleMaterial &&
          asset.retention.retainPalette2 == settings.retention.retainPalette2 &&
          asset.retention.retainPalette4 == settings.retention.retainPalette4 &&
          asset.retention.retainCanonicalSource == settings.retention.retainCanonicalSource)) {
        result.reason = "retention profile changed; perform a full recook";
        return result;
    }

    std::set<BrickKey> keys;
    for (const auto& edit : edits) keys.insert(edit.key);
    result.uniqueBrickEdits = keys.size();
    const auto materialRecords = make_brick_palette_material_records(materials);

    for (const BrickKey key : keys) {
        auto existing = std::lower_bound(asset.bricks.begin(), asset.bricks.end(), key,
            [](const CookedBrickMaterialRepresentations& brick, BrickKey candidate) {
                return brick.key < candidate;
            });
        const Brick* source = object.find_brick(key);
        if (source == nullptr || source->empty()) {
            if (existing != asset.bricks.end() && existing->key == key) {
                asset.bricks.erase(existing);
                ++result.removedBricks;
            } else {
                ++result.unchangedBricks;
            }
            continue;
        }
        auto cooked = cook_one(object, key, *source, materialRecords, settings);
        if (existing != asset.bricks.end() && existing->key == key) {
            if (existing->dependencyKey == cooked.dependencyKey) {
                ++result.unchangedBricks;
                continue;
            }
            *existing = std::move(cooked);
        } else {
            asset.bricks.insert(existing, std::move(cooked));
        }
        ++result.recookedBricks;
    }

    if (result.recookedBricks != 0U || result.removedBricks != 0U) ++asset.generation;
    asset.dependencyKey = brick_palette_asset_dependency_key(object, materials, settings);
    rebuild_asset_metadata(asset);
    result.valid = true;
    result.generation = asset.generation;
    result.reason = "regional brick-palette recook completed";
    return result;
}

const CookedBrickMaterialRepresentations* find_cooked_brick_materials(
    const CookedBrickPaletteAsset& asset, BrickKey key) noexcept {
    const auto it = std::lower_bound(asset.bricks.begin(), asset.bricks.end(), key,
        [](const CookedBrickMaterialRepresentations& brick, BrickKey candidate) {
            return brick.key < candidate;
        });
    return it != asset.bricks.end() && it->key == key ? &*it : nullptr;
}

bool validate_cooked_brick_palette_asset(
    const CookedBrickPaletteAsset& asset, std::string* error) {
    if (asset.generation == 0U) {
        set_error(error, "asset generation is zero");
        return false;
    }
    for (std::size_t index = 0U; index < asset.bricks.size(); ++index) {
        const auto& brick = asset.bricks[index];
        if (index != 0U && !(asset.bricks[index - 1U].key < brick.key)) {
            set_error(error, "cooked brick keys are not strictly sorted");
            return false;
        }
        if (brick.representations.generation != brick.sourceGeneration) {
            set_error(error, "representation generation differs from source brick generation");
            return false;
        }
        if (asset.retention.retainCanonicalSource !=
            brick.representations.canonicalSource.has_value()) {
            set_error(error, "canonical-source retention does not match the asset profile");
            return false;
        }
        if (brick.dependencyKey == 0U) {
            set_error(error, "brick dependency key is zero");
            return false;
        }
    }
    return true;
}

} // namespace dve
