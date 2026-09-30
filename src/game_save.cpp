#include "dve/game_save.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <set>
#include <type_traits>
#include <utility>
#include <variant>

#include "dve/dvox.hpp"
#include "dve/polygon_asset.hpp"
#include "dve/version.hpp"

namespace dve {
namespace {

constexpr std::string_view kMetaSection = "dve.meta";
constexpr std::string_view kWorldSection = "dve.world";
constexpr std::string_view kVoxelSection = "dve.voxels";
constexpr std::string_view kPhysicsSection = "dve.physics";
constexpr std::string_view kScriptSection = "dve.script";
constexpr std::string_view kReservedPrefix = "dve.";

constexpr std::uint8_t kFlagDynamic = 1U << 0U;
constexpr std::uint8_t kFlagStructural = 1U << 1U;
constexpr std::uint8_t kFlagVisualOnly = 1U << 2U;
constexpr std::uint8_t kFlagHasBody = 1U << 3U;
constexpr std::uint8_t kFlagEnabled = 1U << 4U;

constexpr std::uint8_t kMaterialsInline = 0U;
constexpr std::uint8_t kMaterialsFromSource = 1U;

constexpr std::uint8_t kVoxelsFull = 0U;
constexpr std::uint8_t kVoxelsDelta = 1U;

constexpr std::uint8_t kBrickRaw = 0U;
constexpr std::uint8_t kBrickRuns = 1U;

bool fail(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return false;
}

class Writer {
public:
    explicit Writer(const GameSaveLimits& limits) : limits_(limits) {}
    void u8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }
    void u16(std::uint16_t value) { for (unsigned s = 0; s < 16U; s += 8U) u8(static_cast<std::uint8_t>(value >> s)); }
    void u32(std::uint32_t value) { for (unsigned s = 0; s < 32U; s += 8U) u8(static_cast<std::uint8_t>(value >> s)); }
    void u64(std::uint64_t value) { for (unsigned s = 0; s < 64U; s += 8U) u8(static_cast<std::uint8_t>(value >> s)); }
    void i32(std::int32_t value) { u32(std::bit_cast<std::uint32_t>(value)); }
    void i64(std::int64_t value) { u64(std::bit_cast<std::uint64_t>(value)); }
    void f32(float value) { u32(std::bit_cast<std::uint32_t>(value)); }
    void f64(double value) { u64(std::bit_cast<std::uint64_t>(value)); }
    void boolean(bool value) { u8(value ? 1U : 0U); }
    void float3(Float3 value) { f32(value.x); f32(value.y); f32(value.z); }
    void float4(Float4 value) { f32(value.x); f32(value.y); f32(value.z); f32(value.w); }
    void quat(Quaternion value) { f32(value.x); f32(value.y); f32(value.z); f32(value.w); }
    void transform(const RigidTransform& value) { float3(value.position); quat(value.rotation); }
    void string(std::string_view text) {
        if (text.size() > limits_.maximumStringBytes && !failed_) {
            failed_ = true;
            failure_ = "string of " + std::to_string(text.size()) + " bytes is over the save limit";
        }
        u32(static_cast<std::uint32_t>(text.size()));
        for (const char c : text) u8(static_cast<std::uint8_t>(c));
    }
    void count(std::size_t value, std::uint64_t limit, std::string_view what) {
        if (value > limit && !failed_) {
            failed_ = true;
            failure_ = std::string(what) + " count " + std::to_string(value) + " is over the save limit of " + std::to_string(limit);
        }
        u32(static_cast<std::uint32_t>(std::min<std::size_t>(value, 0xFFFFFFFFU)));
    }
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }
    [[nodiscard]] std::vector<std::byte>& bytes() noexcept { return bytes_; }
private:
    const GameSaveLimits& limits_;
    std::vector<std::byte> bytes_;
    bool failed_{};
    std::string failure_;
};

class Reader {
public:
    Reader(std::span<const std::byte> bytes, std::string_view section, const GameSaveLimits& limits)
        : bytes_(bytes), section_(section), limits_(limits) {}
    bool u8(std::uint8_t& value) {
        if (!need(1U)) return false;
        value = std::to_integer<std::uint8_t>(bytes_[offset_++]);
        return true;
    }
    bool u16(std::uint16_t& value) { return integer(value); }
    bool u32(std::uint32_t& value) { return integer(value); }
    bool u64(std::uint64_t& value) { return integer(value); }
    bool i32(std::int32_t& value) { std::uint32_t raw{}; if (!u32(raw)) return false; value = std::bit_cast<std::int32_t>(raw); return true; }
    bool i64(std::int64_t& value) { std::uint64_t raw{}; if (!u64(raw)) return false; value = std::bit_cast<std::int64_t>(raw); return true; }
    bool f32(float& value) { std::uint32_t raw{}; if (!u32(raw)) return false; value = std::bit_cast<float>(raw); return true; }
    bool f64(double& value) { std::uint64_t raw{}; if (!u64(raw)) return false; value = std::bit_cast<double>(raw); return true; }
    bool boolean(bool& value) {
        std::uint8_t raw{};
        if (!u8(raw)) return false;
        if (raw > 1U) return bad("invalid boolean");
        value = raw == 1U;
        return true;
    }
    bool float3(Float3& v) { return f32(v.x) && f32(v.y) && f32(v.z); }
    bool float4(Float4& v) { return f32(v.x) && f32(v.y) && f32(v.z) && f32(v.w); }
    bool quat(Quaternion& v) { return f32(v.x) && f32(v.y) && f32(v.z) && f32(v.w); }
    bool transform(RigidTransform& v) { return float3(v.position) && quat(v.rotation); }
    bool string(std::string& text) {
        std::uint32_t size{};
        if (!u32(size)) return false;
        if (size > limits_.maximumStringBytes) return bad("string of " + std::to_string(size) + " bytes is over the limit");
        if (!need(size)) return false;
        text.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
        offset_ += size;
        return true;
    }
    bool count(std::uint32_t& value, std::uint64_t limit, std::string_view what) {
        if (!u32(value)) return false;
        if (value > limit) return bad(std::string(what) + " count " + std::to_string(value) + " is over the limit of " + std::to_string(limit));
        // Every element takes at least one byte: a count larger than the rest is a lie.
        if (value > remaining()) return bad(std::string(what) + " count " + std::to_string(value) + " exceeds the section size");
        return true;
    }
    bool raw(std::span<const std::byte>& out, std::size_t size) {
        if (!need(size)) return false;
        out = bytes_.subspan(offset_, size);
        offset_ += size;
        return true;
    }
    bool bad(std::string message) {
        if (error_.empty()) error_ = std::move(message);
        return false;
    }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    [[nodiscard]] bool finish() {
        if (remaining() != 0U) return bad(std::to_string(remaining()) + " unexpected trailing bytes");
        return true;
    }
    [[nodiscard]] std::string message() const {
        return "save section '" + std::string(section_) + "' is corrupt at byte " + std::to_string(offset_) + ": " +
               (error_.empty() ? std::string("truncated") : error_);
    }
private:
    bool need(std::size_t size) {
        if (remaining() < size) return bad("truncated");
        return true;
    }
    template <class T>
    bool integer(T& value) {
        if (!need(sizeof(T))) return false;
        T result{};
        for (std::size_t i = 0; i < sizeof(T); ++i)
            result = static_cast<T>(result | (static_cast<T>(std::to_integer<std::uint8_t>(bytes_[offset_ + i])) << (8U * i)));
        offset_ += sizeof(T);
        value = result;
        return true;
    }
    std::span<const std::byte> bytes_;
    std::size_t offset_{};
    std::string_view section_;
    const GameSaveLimits& limits_;
    std::string error_;
};

// --- materials ------------------------------------------------------------------------------

void write_material(Writer& w, const VoxelMaterialDefinition& m) {
    w.string(m.name);
    w.float4(m.baseColor);
    w.float3(m.emissive);
    w.f32(m.metallic); w.f32(m.roughness); w.f32(m.specular);
    w.u8(static_cast<std::uint8_t>(m.shadingModel));
    w.u8(static_cast<std::uint8_t>(m.blendMode));
    w.f32(m.subsurfaceScatterDistanceMeters); w.float3(m.subsurfaceColor);
    w.f32(m.clearCoat); w.f32(m.clearCoatRoughness);
    w.float3(m.foliageColor); w.f32(m.foliageTransmittance); w.f32(m.foliageWrap);
    w.count(m.layers.size(), kMaximumVoxelMaterialLayers, "material layer");
    for (const VoxelMaterialLayer& layer : m.layers) {
        w.u8(layer.sourceMaterial); w.f32(layer.weight);
        w.u8(static_cast<std::uint8_t>(layer.blendMode)); w.boolean(layer.enabled);
    }
    w.f32(m.densityKilogramsPerCubicMeter); w.f32(m.structuralStrength); w.f32(m.fractureResistance);
    w.f32(m.flammability); w.f32(m.thermalConductivity); w.boolean(m.transparent); w.boolean(m.structural);
}

bool read_material(Reader& r, VoxelMaterialDefinition& m) {
    std::uint8_t shading{}, blend{};
    std::uint32_t layers{};
    if (!(r.string(m.name) && r.float4(m.baseColor) && r.float3(m.emissive) && r.f32(m.metallic) &&
          r.f32(m.roughness) && r.f32(m.specular) && r.u8(shading) && r.u8(blend) &&
          r.f32(m.subsurfaceScatterDistanceMeters) && r.float3(m.subsurfaceColor) && r.f32(m.clearCoat) &&
          r.f32(m.clearCoatRoughness) && r.float3(m.foliageColor) && r.f32(m.foliageTransmittance) &&
          r.f32(m.foliageWrap) && r.count(layers, kMaximumVoxelMaterialLayers, "material layer")))
        return false;
    if (shading > static_cast<std::uint8_t>(MaterialShadingModel::ClearCoat) ||
        blend > static_cast<std::uint8_t>(MaterialBlendMode::Translucent))
        return r.bad("invalid material enum");
    m.shadingModel = static_cast<MaterialShadingModel>(shading);
    m.blendMode = static_cast<MaterialBlendMode>(blend);
    m.layers.resize(layers);
    for (VoxelMaterialLayer& layer : m.layers) {
        std::uint8_t mode{};
        if (!(r.u8(layer.sourceMaterial) && r.f32(layer.weight) && r.u8(mode) && r.boolean(layer.enabled))) return false;
        if (mode > static_cast<std::uint8_t>(MaterialLayerBlendMode::Additive)) return r.bad("invalid layer blend mode");
        layer.blendMode = static_cast<MaterialLayerBlendMode>(mode);
    }
    return r.f32(m.densityKilogramsPerCubicMeter) && r.f32(m.structuralStrength) && r.f32(m.fractureResistance) &&
           r.f32(m.flammability) && r.f32(m.thermalConductivity) && r.boolean(m.transparent) && r.boolean(m.structural);
}

std::vector<std::byte> material_bytes(const std::vector<VoxelMaterialDefinition>& materials) {
    GameSaveLimits unlimited;
    unlimited.maximumStringBytes = 0xFFFFFFFFU;
    Writer w(unlimited);
    w.u32(static_cast<std::uint32_t>(materials.size()));
    for (const auto& m : materials) write_material(w, m);
    return std::move(w.bytes());
}

// --- bricks ---------------------------------------------------------------------------------

void write_brick(Writer& w, const GameWorldBrickState& brick) {
    w.i32(brick.key.x); w.i32(brick.key.y); w.i32(brick.key.z);
    w.u32(brick.generation);
    std::vector<std::pair<std::uint8_t, MaterialId>> runs;   // (length - 1, material)
    for (std::size_t i = 0; i < brick.materials.size();) {
        std::size_t j = i + 1U;
        while (j < brick.materials.size() && brick.materials[j] == brick.materials[i] && j - i < 256U) ++j;
        runs.emplace_back(static_cast<std::uint8_t>(j - i - 1U), brick.materials[i]);
        i = j;
    }
    if (2U + runs.size() * 2U < brick.materials.size()) {
        w.u8(kBrickRuns);
        w.u16(static_cast<std::uint16_t>(runs.size()));
        for (const auto& [length, material] : runs) { w.u8(length); w.u8(material); }
    } else {
        w.u8(kBrickRaw);
        for (const MaterialId material : brick.materials) w.u8(material);
    }
}

bool read_brick(Reader& r, GameWorldBrickState& brick) {
    std::uint8_t mode{};
    if (!(r.i32(brick.key.x) && r.i32(brick.key.y) && r.i32(brick.key.z) && r.u32(brick.generation) && r.u8(mode)))
        return false;
    if (mode == kBrickRaw) {
        std::span<const std::byte> raw;
        if (!r.raw(raw, brick.materials.size())) return false;
        for (std::size_t i = 0; i < raw.size(); ++i) brick.materials[i] = std::to_integer<MaterialId>(raw[i]);
        return true;
    }
    if (mode != kBrickRuns) return r.bad("unknown brick encoding " + std::to_string(mode));
    std::uint16_t runs{};
    if (!r.u16(runs)) return false;
    if (runs == 0U || runs > brick.materials.size()) return r.bad("invalid brick run count");
    std::size_t offset = 0U;
    for (std::uint16_t i = 0; i < runs; ++i) {
        std::uint8_t length{}, material{};
        if (!(r.u8(length) && r.u8(material))) return false;
        const std::size_t count = static_cast<std::size_t>(length) + 1U;
        if (offset + count > brick.materials.size()) return r.bad("brick runs overflow the brick");
        std::fill_n(brick.materials.begin() + static_cast<std::ptrdiff_t>(offset), count, material);
        offset += count;
    }
    if (offset != brick.materials.size()) return r.bad("brick runs do not cover the brick");
    return true;
}

// --- components -----------------------------------------------------------------------------

void write_component(Writer& w, const Component& component, const GameSaveLimits& limits) {
    w.u64(component.id);
    w.string(component.type);
    w.boolean(component.enabled);
    w.count(component.properties.size(), limits.maximumPropertiesPerComponent, "component property");
    for (const auto& [key, value] : component.properties) {
        w.string(key);
        w.u8(static_cast<std::uint8_t>(value.index()));
        std::visit([&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, bool>) w.boolean(item);
            else if constexpr (std::is_same_v<T, std::int64_t>) w.i64(item);
            else if constexpr (std::is_same_v<T, double>) w.f64(item);
            else if constexpr (std::is_same_v<T, std::string>) w.string(item);
            else if constexpr (std::is_same_v<T, Float3>) w.float3(item);
            else w.quat(item);
        }, value);
    }
}

bool read_component(Reader& r, Component& component, const GameSaveLimits& limits) {
    std::uint32_t properties{};
    if (!(r.u64(component.id) && r.string(component.type) && r.boolean(component.enabled) &&
          r.count(properties, limits.maximumPropertiesPerComponent, "component property")))
        return false;
    for (std::uint32_t i = 0; i < properties; ++i) {
        std::string key;
        std::uint8_t index{};
        if (!(r.string(key) && r.u8(index))) return false;
        ComponentValue value;
        bool ok = false;
        switch (index) {
        case 0: { bool v{}; ok = r.boolean(v); value = v; break; }
        case 1: { std::int64_t v{}; ok = r.i64(v); value = v; break; }
        case 2: { double v{}; ok = r.f64(v); value = v; break; }
        case 3: { std::string v; ok = r.string(v); value = std::move(v); break; }
        case 4: { Float3 v{}; ok = r.float3(v); value = v; break; }
        case 5: { Quaternion v{}; ok = r.quat(v); value = v; break; }
        default: return r.bad("unknown component value type");
        }
        if (!ok) return false;
        if (!component.properties.emplace(std::move(key), std::move(value)).second) return r.bad("duplicate component property");
    }
    return true;
}

// --- sources --------------------------------------------------------------------------------

struct DecodedSource {
    std::uint64_t hash{};
    std::optional<CookedVoxelAsset> voxel;
    std::shared_ptr<const CookedPolygonAsset> polygon;
};

bool is_polygon_path(std::string_view path) {
    return path.size() >= 6U && (path.ends_with(".dmesh") || path.ends_with(".DMESH"));
}

class SourceCache {
public:
    SourceCache(const GameSaveSourceReader& reader, const GameSaveLimits& limits) : reader_(reader), limits_(limits) {}
    const DecodedSource* get(const GameObjectSource& source, std::string* error) {
        if (const auto it = cache_.find(source.path); it != cache_.end()) {
            if (it->second.hash != source.contentHash) {
                fail(error, "source asset '" + source.path + "' does not match the save (the game content changed)");
                return nullptr;
            }
            return &it->second;
        }
        if (!reader_) {
            fail(error, "no source reader to resolve asset '" + source.path + "'");
            return nullptr;
        }
        std::string readError;
        auto bytes = reader_(source.path, limits_.maximumSourceAssetBytes, &readError);
        if (!bytes) {
            fail(error, "cannot read source asset '" + source.path + "': " + readError);
            return nullptr;
        }
        DecodedSource decoded;
        decoded.hash = game_save_content_hash(*bytes);
        if (decoded.hash != source.contentHash) {
            fail(error, "source asset '" + source.path + "' does not match the save (the game content changed)");
            return nullptr;
        }
        if (is_polygon_path(source.path)) {
            PolygonAssetReadResult result = read_dmesh(*bytes);
            if (!result) {
                fail(error, "source asset '" + source.path + "': " + result.error);
                return nullptr;
            }
            decoded.polygon = std::make_shared<const CookedPolygonAsset>(std::move(result.asset));
        } else {
            DvoxReadResult result = read_dvox(*bytes, limits_.maximumSourceAssetBytes);
            if (!result.success) {
                fail(error, "source asset '" + source.path + "': " + result.error);
                return nullptr;
            }
            decoded.voxel.emplace(std::move(result.asset));
        }
        return &cache_.emplace(source.path, std::move(decoded)).first->second;
    }
private:
    const GameSaveSourceReader& reader_;
    const GameSaveLimits& limits_;
    std::map<std::string, DecodedSource, std::less<>> cache_;
};

std::vector<GameWorldBrickState> bricks_of(const VoxelObject& voxels) {
    std::vector<GameWorldBrickState> bricks;
    bricks.reserve(voxels.brick_count());
    for (const auto& [key, brick] : voxels.bricks()) bricks.push_back({key, brick.generation(), brick.materials()});
    return bricks;
}

bool same_brick(const GameWorldBrickState& a, const GameWorldBrickState& b) {
    return a.key == b.key && a.generation == b.generation && a.materials == b.materials;
}

} // namespace

std::uint64_t game_save_content_hash(std::span<const std::byte> bytes) noexcept {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::byte value : bytes) {
        hash ^= std::to_integer<std::uint8_t>(value);
        hash *= 1099511628211ULL;
    }
    return hash;
}

GameSaveCodec::GameSaveCodec(GameSaveSourceReader sources, GameSaveLimits limits)
    : sources_(std::move(sources)), limits_(limits),
      store_(kGameSaveSchemaVersion, SaveGameLimits{limits.maximumFileBytes, 64U + limits.maximumGameSections, 256U,
                                                    limits.maximumFileBytes}) {}

bool GameSaveCodec::register_migration(std::uint32_t fromVersion, SaveGameMigration migration, std::string* error) {
    return store_.register_migration(fromVersion, std::move(migration), error);
}

std::optional<SaveGameDocument> GameSaveCodec::to_document(
    const GameSaveData& data, GameSaveStats* stats, std::string* error) const {
    GameSaveStats local;
    const GameWorldSaveState& world = data.world;
    if (world.objects.size() > limits_.maximumObjects)
        return fail(error, "the world has " + std::to_string(world.objects.size()) + " objects, over the save limit of " +
                               std::to_string(limits_.maximumObjects)), std::nullopt;
    if (data.scriptState && data.scriptState->size() > limits_.maximumScriptStateBytes)
        return fail(error, "script state is " + std::to_string(data.scriptState->size()) + " bytes, over the save limit"),
               std::nullopt;
    if (data.gameSections.size() > limits_.maximumGameSections) return fail(error, "too many game save sections"), std::nullopt;
    for (const auto& [name, bytes] : data.gameSections) {
        (void)bytes;
        if (name.empty() || name.size() > 64U || name.starts_with(kReservedPrefix))
            return fail(error, "invalid game save section name '" + name + "' (1-64 bytes, not starting with 'dve.')"),
                   std::nullopt;
    }

    SourceCache sources(sources_, limits_);
    Writer meta(limits_), objects(limits_), voxels(limits_), physics(limits_);

    meta.string(kVersionString);
    meta.string(data.metadata.gameName);
    meta.string(data.metadata.gameVersion);
    meta.string(data.metadata.scenePath);
    meta.u64(data.metadata.tickCount);
    meta.u64(data.metadata.worldStateHash);
    meta.count(data.metadata.info.size(), 256U, "metadata entry");
    for (const auto& [key, value] : data.metadata.info) { meta.string(key); meta.string(value); }

    objects.u64(world.nextObjectId);
    objects.u64(world.nextPoolId);
    objects.u64(world.nextTimerId);
    objects.f32(world.elapsedSeconds);
    objects.count(world.objects.size(), limits_.maximumObjects, "object");
    std::size_t voxelObjects = 0U;
    std::size_t bodies = 0U;
    std::uint64_t totalBricks = 0U;
    for (const GameWorldObjectState& object : world.objects) {
        if (object.kind == GameGeometryKind::Voxel) ++voxelObjects;
        if (object.hasBody) ++bodies;
    }
    voxels.count(voxelObjects, limits_.maximumObjects, "voxel object");
    physics.count(bodies, limits_.maximumObjects, "body");
    for (const GameWorldObjectState& object : world.objects) {
        const std::string label = "object " + std::to_string(object.id) + " ('" + object.name + "')";
        if (object.kind != GameGeometryKind::Marker && object.kind != GameGeometryKind::Voxel &&
            object.kind != GameGeometryKind::Polygon)
            return fail(error, label + " has an unsupported geometry kind"), std::nullopt;
        if (object.kind == GameGeometryKind::Polygon && !object.source)
            return fail(error, label + " is a polygon object without a source asset; it cannot be saved"), std::nullopt;
        objects.u64(object.id);
        objects.string(object.name);
        objects.count(object.tags.size(), limits_.maximumTagsPerObject, "tag");
        for (const auto& tag : object.tags) objects.string(tag);
        objects.count(object.groups.size(), limits_.maximumTagsPerObject, "group");
        for (const auto& group : object.groups) objects.string(group);
        objects.u32(object.layer);
        objects.count(object.components.size(), limits_.maximumComponentsPerObject, "component");
        for (const Component& component : object.components) write_component(objects, component, limits_);
        std::uint8_t flags = 0U;
        if (object.dynamic) flags |= kFlagDynamic;
        if (object.structural) flags |= kFlagStructural;
        if (object.visualOnly) flags |= kFlagVisualOnly;
        if (object.hasBody) flags |= kFlagHasBody;
        if (object.enabled) flags |= kFlagEnabled;
        objects.u8(flags);
        objects.u8(static_cast<std::uint8_t>(object.kind));
        objects.boolean(object.attachment.has_value());
        if (object.attachment) {
            objects.u64(object.attachment->parent);
            objects.transform(object.attachment->localTransform);
            objects.string(object.attachment->socket);
            objects.boolean(object.attachment->inheritPosition);
            objects.boolean(object.attachment->inheritRotation);
        }
        objects.transform(object.authoredTransform);
        objects.f32(object.voxelSizeMeters);
        objects.boolean(object.source.has_value());
        if (object.source) {
            objects.string(object.source->path);
            objects.u64(object.source->contentHash);
            objects.boolean(object.source->derived);
        }
        objects.boolean(object.pool.has_value());
        if (object.pool) objects.u64(*object.pool);
        // Density table (256 x u16): run-length encoded as u32 runs x {u8 length-1, u16 units}.
        {
            std::vector<std::pair<std::uint8_t, std::uint16_t>> runs;
            for (std::size_t i = 0; i < object.densityUnits.size();) {
                std::size_t j = i + 1U;
                while (j < object.densityUnits.size() && object.densityUnits[j] == object.densityUnits[i]) ++j;
                runs.emplace_back(static_cast<std::uint8_t>(j - i - 1U), object.densityUnits[i]);
                i = j;
            }
            objects.u32(static_cast<std::uint32_t>(runs.size()));
            for (const auto& [length, units] : runs) { objects.u8(length); objects.u16(units); }
        }
        objects.f64(object.densityQuantumKilogramsPerCubicMeter);

        // Material table: from the source when identical, else inline.
        const DecodedSource* decoded = nullptr;
        if (object.source) {
            std::string sourceError;
            decoded = sources.get(*object.source, &sourceError);
            if (!decoded) return fail(error, label + ": " + sourceError), std::nullopt;
        }
        const std::vector<VoxelMaterialDefinition>* sourceMaterials =
            decoded && decoded->voxel ? &decoded->voxel->materials : nullptr;
        if (object.kind == GameGeometryKind::Voxel && sourceMaterials &&
            material_bytes(*sourceMaterials) == material_bytes(object.materials)) {
            objects.u8(kMaterialsFromSource);
        } else {
            objects.u8(kMaterialsInline);
            objects.count(object.materials.size(), limits_.maximumMaterialsPerObject, "material");
            for (const auto& material : object.materials) write_material(objects, material);
        }

        if (object.kind == GameGeometryKind::Voxel) {
            ++local.voxelObjects;
            voxels.u64(object.id);
            voxels.u64(object.voxelObjectId);
            // Delta against the source when this object *is* the source asset (not a fragment)
            // and its brick storage still starts with the source's bricks in order.
            std::optional<std::vector<const GameWorldBrickState*>> delta;
            std::vector<GameWorldBrickState> sourceBricks;
            if (decoded && decoded->voxel && !object.source->derived &&
                decoded->voxel->object.id() == object.voxelObjectId) {
                sourceBricks = bricks_of(decoded->voxel->object);
                if (sourceBricks.size() <= object.bricks.size()) {
                    std::vector<const GameWorldBrickState*> changed;
                    bool prefix = true;
                    for (std::size_t i = 0; i < object.bricks.size() && prefix; ++i) {
                        if (i < sourceBricks.size()) {
                            if (object.bricks[i].key != sourceBricks[i].key) prefix = false;
                            else if (!same_brick(object.bricks[i], sourceBricks[i])) changed.push_back(&object.bricks[i]);
                        } else {
                            changed.push_back(&object.bricks[i]);
                        }
                    }
                    if (prefix) delta = std::move(changed);
                }
            }
            if (delta) {
                voxels.u8(kVoxelsDelta);
                voxels.u32(static_cast<std::uint32_t>(sourceBricks.size()));
                voxels.u32(static_cast<std::uint32_t>(delta->size()));
                // Each record carries its storage index so new bricks keep their order.
                for (const GameWorldBrickState* brick : *delta) {
                    voxels.u32(static_cast<std::uint32_t>(brick - object.bricks.data()));
                    write_brick(voxels, *brick);
                }
                ++local.deltaObjects;
                local.deltaBricks += delta->size();
                local.unchangedBricks += object.bricks.size() - delta->size();
                totalBricks += object.bricks.size();
            } else {
                voxels.u8(kVoxelsFull);
                voxels.u32(static_cast<std::uint32_t>(object.bricks.size()));
                for (const GameWorldBrickState& brick : object.bricks) write_brick(voxels, brick);
                ++local.fullObjects;
                local.fullBricks += object.bricks.size();
                totalBricks += object.bricks.size();
            }
        }
        if (object.hasBody) {
            physics.u64(object.id);
            physics.transform(object.body.previousTransform);
            physics.transform(object.body.currentTransform);
            physics.float3(object.body.linearVelocity);
            physics.float3(object.body.angularVelocity);
            physics.boolean(object.body.sleeping);
            physics.float3(object.localCenterOfMassMeters);
        }
    }
    if (totalBricks > limits_.maximumTotalBricks)
        return fail(error, "the world has " + std::to_string(totalBricks) + " voxel bricks, over the save limit of " +
                               std::to_string(limits_.maximumTotalBricks)), std::nullopt;
    objects.count(world.timers.size(), limits_.maximumTimers, "timer");
    for (const GameWorldTimerState& timer : world.timers) {
        objects.u64(timer.id); objects.f32(timer.fireAtSeconds); objects.f32(timer.intervalSeconds);
    }
    objects.count(world.pools.size(), limits_.maximumPools, "pool");
    for (const GameWorldPoolState& pool : world.pools) {
        objects.u64(pool.id); objects.string(pool.name); objects.u64(pool.capacity);
        objects.count(pool.freeIds.size(), limits_.maximumObjects, "pool slot");
        for (const GameObjectId id : pool.freeIds) objects.u64(id);
    }
    for (const Writer* writer : {&meta, &objects, &voxels, &physics}) {
        if (writer->failed()) return fail(error, writer->failure()), std::nullopt;
    }

    SaveGameDocument document;
    document.schemaVersion = kGameSaveSchemaVersion;
    document.sequence = data.metadata.tickCount;
    document.sections.emplace(std::string(kMetaSection), std::move(meta.bytes()));
    document.sections.emplace(std::string(kWorldSection), std::move(objects.bytes()));
    document.sections.emplace(std::string(kVoxelSection), std::move(voxels.bytes()));
    document.sections.emplace(std::string(kPhysicsSection), std::move(physics.bytes()));
    if (data.scriptState) document.sections.emplace(std::string(kScriptSection), *data.scriptState);
    for (const auto& [name, bytes] : data.gameSections) document.sections.emplace(name, bytes);
    local.objects = world.objects.size();
    std::uint64_t total = 8U + 4U + 8U + 4U + 8U;
    for (const auto& [name, bytes] : document.sections) {
        local.sectionBytes[name] = bytes.size();
        total += 4U + 8U + 8U + name.size() + bytes.size();
    }
    local.fileBytes = total;
    if (total > limits_.maximumFileBytes)
        return fail(error, "the save would be " + std::to_string(total) + " bytes, over the " +
                               std::to_string(limits_.maximumFileBytes) + "-byte limit"), std::nullopt;
    if (stats) *stats = std::move(local);
    return document;
}

std::optional<GameSaveData> GameSaveCodec::from_document(const SaveGameDocument& document, std::string* error) const {
    if (document.schemaVersion != kGameSaveSchemaVersion)
        return fail(error, "save document is schema version " + std::to_string(document.schemaVersion) +
                               ", expected " + std::to_string(kGameSaveSchemaVersion)), std::nullopt;
    const auto section = [&](std::string_view name) -> const std::vector<std::byte>* {
        const auto it = document.sections.find(name);
        return it == document.sections.end() ? nullptr : &it->second;
    };
    for (const std::string_view required : {kMetaSection, kWorldSection, kVoxelSection, kPhysicsSection}) {
        if (!section(required)) return fail(error, "save is missing section '" + std::string(required) + "'"), std::nullopt;
    }
    GameSaveData data;
    const auto corrupt = [&](const Reader& reader) { return fail(error, reader.message()), std::nullopt; };

    {
        Reader r(*section(kMetaSection), kMetaSection, limits_);
        std::uint32_t entries{};
        if (!(r.string(data.metadata.engineVersion) && r.string(data.metadata.gameName) &&
              r.string(data.metadata.gameVersion) && r.string(data.metadata.scenePath) &&
              r.u64(data.metadata.tickCount) && r.u64(data.metadata.worldStateHash) && r.count(entries, 256U, "metadata entry")))
            return corrupt(r);
        for (std::uint32_t i = 0; i < entries; ++i) {
            std::string key, value;
            if (!(r.string(key) && r.string(value))) return corrupt(r);
            data.metadata.info[std::move(key)] = std::move(value);
        }
        if (!r.finish()) return corrupt(r);
    }

    SourceCache sources(sources_, limits_);
    GameWorldSaveState& world = data.world;
    std::map<GameObjectId, std::size_t> indexById;
    std::vector<bool> materialsFromSource;
    {
        Reader r(*section(kWorldSection), kWorldSection, limits_);
        std::uint32_t objectCount{};
        if (!(r.u64(world.nextObjectId) && r.u64(world.nextPoolId) && r.u64(world.nextTimerId) &&
              r.f32(world.elapsedSeconds) && r.count(objectCount, limits_.maximumObjects, "object")))
            return corrupt(r);
        world.objects.resize(objectCount);
        materialsFromSource.resize(objectCount);
        for (std::uint32_t index = 0; index < objectCount; ++index) {
            GameWorldObjectState& object = world.objects[index];
            std::uint32_t tags{}, groups{}, components{}, densityEntries{};
            std::uint8_t flags{}, kind{}, materialsMode{};
            bool hasAttachment{}, hasSource{}, hasPool{};
            if (!(r.u64(object.id) && r.string(object.name) && r.count(tags, limits_.maximumTagsPerObject, "tag")))
                return corrupt(r);
            object.tags.resize(tags);
            for (auto& tag : object.tags) if (!r.string(tag)) return corrupt(r);
            if (!r.count(groups, limits_.maximumTagsPerObject, "group")) return corrupt(r);
            object.groups.resize(groups);
            for (auto& group : object.groups) if (!r.string(group)) return corrupt(r);
            if (!(r.u32(object.layer) && r.count(components, limits_.maximumComponentsPerObject, "component")))
                return corrupt(r);
            object.components.resize(components);
            for (Component& component : object.components) if (!read_component(r, component, limits_)) return corrupt(r);
            if (!(r.u8(flags) && r.u8(kind) && r.boolean(hasAttachment))) return corrupt(r);
            if (flags > 0x1FU) { r.bad("unknown object flags"); return corrupt(r); }
            if (kind != static_cast<std::uint8_t>(GameGeometryKind::Marker) &&
                kind != static_cast<std::uint8_t>(GameGeometryKind::Voxel) &&
                kind != static_cast<std::uint8_t>(GameGeometryKind::Polygon)) { r.bad("unknown geometry kind"); return corrupt(r); }
            object.kind = static_cast<GameGeometryKind>(kind);
            object.dynamic = (flags & kFlagDynamic) != 0U;
            object.structural = (flags & kFlagStructural) != 0U;
            object.visualOnly = (flags & kFlagVisualOnly) != 0U;
            object.hasBody = (flags & kFlagHasBody) != 0U;
            object.enabled = (flags & kFlagEnabled) != 0U;
            if (hasAttachment) {
                GameObjectAttachment attachment;
                if (!(r.u64(attachment.parent) && r.transform(attachment.localTransform) && r.string(attachment.socket) &&
                      r.boolean(attachment.inheritPosition) && r.boolean(attachment.inheritRotation)))
                    return corrupt(r);
                object.attachment = std::move(attachment);
            }
            if (!(r.transform(object.authoredTransform) && r.f32(object.voxelSizeMeters) && r.boolean(hasSource)))
                return corrupt(r);
            if (hasSource) {
                GameObjectSource source;
                if (!(r.string(source.path) && r.u64(source.contentHash) && r.boolean(source.derived))) return corrupt(r);
                object.source = std::move(source);
            }
            if (!r.boolean(hasPool)) return corrupt(r);
            if (hasPool) {
                GameObjectPoolId pool{};
                if (!r.u64(pool)) return corrupt(r);
                object.pool = pool;
            }
            if (!r.count(densityEntries, 256U, "density run")) return corrupt(r);
            {
                std::size_t filled = 0U;
                for (std::uint32_t i = 0; i < densityEntries; ++i) {
                    std::uint8_t length{};
                    std::uint16_t units{};
                    if (!(r.u8(length) && r.u16(units))) return corrupt(r);
                    const std::size_t count = static_cast<std::size_t>(length) + 1U;
                    if (filled + count > object.densityUnits.size()) { r.bad("density runs overflow the table"); return corrupt(r); }
                    std::fill_n(object.densityUnits.begin() + static_cast<std::ptrdiff_t>(filled), count, units);
                    filled += count;
                }
                if (filled != object.densityUnits.size()) { r.bad("density runs do not cover the table"); return corrupt(r); }
            }
            if (!(r.f64(object.densityQuantumKilogramsPerCubicMeter) && r.u8(materialsMode))) return corrupt(r);
            if (materialsMode == kMaterialsFromSource) {
                if (!object.source) { r.bad("materials refer to a missing source"); return corrupt(r); }
                materialsFromSource[index] = true;
            } else if (materialsMode == kMaterialsInline) {
                std::uint32_t materials{};
                if (!r.count(materials, limits_.maximumMaterialsPerObject, "material")) return corrupt(r);
                object.materials.resize(materials);
                for (auto& material : object.materials) if (!read_material(r, material)) return corrupt(r);
            } else {
                r.bad("unknown material table mode");
                return corrupt(r);
            }
            if (!indexById.emplace(object.id, index).second) { r.bad("duplicate object id"); return corrupt(r); }
        }
        std::uint32_t timers{}, pools{};
        if (!r.count(timers, limits_.maximumTimers, "timer")) return corrupt(r);
        world.timers.resize(timers);
        for (auto& timer : world.timers)
            if (!(r.u64(timer.id) && r.f32(timer.fireAtSeconds) && r.f32(timer.intervalSeconds))) return corrupt(r);
        if (!r.count(pools, limits_.maximumPools, "pool")) return corrupt(r);
        world.pools.resize(pools);
        for (auto& pool : world.pools) {
            std::uint32_t slots{};
            if (!(r.u64(pool.id) && r.string(pool.name) && r.u64(pool.capacity) &&
                  r.count(slots, limits_.maximumObjects, "pool slot")))
                return corrupt(r);
            pool.freeIds.resize(slots);
            for (auto& id : pool.freeIds) if (!r.u64(id)) return corrupt(r);
        }
        if (!r.finish()) return corrupt(r);
    }

    // Sources: material tables and polygon geometry.
    for (std::size_t index = 0; index < world.objects.size(); ++index) {
        GameWorldObjectState& object = world.objects[index];
        if (!object.source || (!materialsFromSource[index] && object.kind != GameGeometryKind::Polygon)) continue;
        std::string sourceError;
        const DecodedSource* decoded = sources.get(*object.source, &sourceError);
        const std::string label = "object " + std::to_string(object.id) + " ('" + object.name + "')";
        if (!decoded) return fail(error, label + ": " + sourceError), std::nullopt;
        if (object.kind == GameGeometryKind::Polygon) {
            if (!decoded->polygon) return fail(error, label + ": source asset is not a polygon asset"), std::nullopt;
            object.polygon = decoded->polygon;
        }
        if (materialsFromSource[index]) {
            if (!decoded->voxel) return fail(error, label + ": source asset is not a voxel asset"), std::nullopt;
            object.materials = decoded->voxel->materials;
        }
    }

    {
        Reader r(*section(kVoxelSection), kVoxelSection, limits_);
        std::uint32_t count{};
        if (!r.count(count, limits_.maximumObjects, "voxel object")) return corrupt(r);
        std::uint64_t totalBricks = 0U;
        std::set<GameObjectId> seen;
        for (std::uint32_t i = 0; i < count; ++i) {
            GameObjectId id{};
            std::uint64_t voxelObjectId{};
            std::uint8_t mode{};
            if (!(r.u64(id) && r.u64(voxelObjectId) && r.u8(mode))) return corrupt(r);
            const auto found = indexById.find(id);
            if (found == indexById.end() || !seen.insert(id).second) { r.bad("voxels for an unknown object"); return corrupt(r); }
            GameWorldObjectState& object = world.objects[found->second];
            if (object.kind != GameGeometryKind::Voxel) { r.bad("voxels for a non-voxel object"); return corrupt(r); }
            object.voxelObjectId = voxelObjectId;
            if (mode == kVoxelsFull) {
                std::uint32_t bricks{};
                if (!r.u32(bricks)) return corrupt(r);
                totalBricks += bricks;
                if (totalBricks > limits_.maximumTotalBricks) { r.bad("brick count is over the limit"); return corrupt(r); }
                if (bricks > r.remaining() / 21U) { r.bad("brick count exceeds the section size"); return corrupt(r); }
                object.bricks.resize(bricks);
                for (auto& brick : object.bricks) if (!read_brick(r, brick)) return corrupt(r);
            } else if (mode == kVoxelsDelta) {
                std::uint32_t sourceCount{}, records{};
                if (!(r.u32(sourceCount) && r.u32(records))) return corrupt(r);
                if (!object.source || object.source->derived) { r.bad("delta without a source asset"); return corrupt(r); }
                if (records > r.remaining() / 25U) { r.bad("delta record count exceeds the section size"); return corrupt(r); }
                std::string sourceError;
                const DecodedSource* decoded = sources.get(*object.source, &sourceError);
                if (!decoded || !decoded->voxel)
                    return fail(error, "object " + std::to_string(object.id) + " ('" + object.name + "'): " +
                                           (decoded ? std::string("source asset is not a voxel asset") : sourceError)),
                           std::nullopt;
                if (decoded->voxel->object.id() != voxelObjectId || decoded->voxel->object.brick_count() != sourceCount)
                    return fail(error, "object " + std::to_string(object.id) + " ('" + object.name +
                                           "'): source asset '" + object.source->path + "' does not match the saved delta"),
                           std::nullopt;
                object.bricks = bricks_of(decoded->voxel->object);
                std::uint32_t previousIndex = 0U;
                for (std::uint32_t k = 0; k < records; ++k) {
                    std::uint32_t storageIndex{};
                    GameWorldBrickState brick;
                    if (!(r.u32(storageIndex) && read_brick(r, brick))) return corrupt(r);
                    if (k > 0U && storageIndex <= previousIndex) { r.bad("delta records out of order"); return corrupt(r); }
                    previousIndex = storageIndex;
                    if (storageIndex < object.bricks.size()) {
                        if (object.bricks[storageIndex].key != brick.key) { r.bad("delta brick key mismatch"); return corrupt(r); }
                        object.bricks[storageIndex] = brick;
                    } else if (storageIndex == object.bricks.size()) {
                        object.bricks.push_back(brick);
                    } else {
                        r.bad("delta brick index skips storage");
                        return corrupt(r);
                    }
                }
                totalBricks += object.bricks.size();
                if (totalBricks > limits_.maximumTotalBricks) { r.bad("brick count is over the limit"); return corrupt(r); }
            } else {
                r.bad("unknown voxel storage mode");
                return corrupt(r);
            }
        }
        for (const GameWorldObjectState& object : world.objects) {
            if (object.kind == GameGeometryKind::Voxel && !seen.contains(object.id))
                return fail(error, "save has no voxels for object " + std::to_string(object.id)), std::nullopt;
        }
        if (!r.finish()) return corrupt(r);
    }

    {
        Reader r(*section(kPhysicsSection), kPhysicsSection, limits_);
        std::uint32_t count{};
        if (!r.count(count, limits_.maximumObjects, "body")) return corrupt(r);
        std::set<GameObjectId> seen;
        for (std::uint32_t i = 0; i < count; ++i) {
            GameObjectId id{};
            if (!r.u64(id)) return corrupt(r);
            const auto found = indexById.find(id);
            if (found == indexById.end() || !seen.insert(id).second) { r.bad("body for an unknown object"); return corrupt(r); }
            GameWorldObjectState& object = world.objects[found->second];
            if (!object.hasBody) { r.bad("body for an object without one"); return corrupt(r); }
            if (!(r.transform(object.body.previousTransform) && r.transform(object.body.currentTransform) &&
                  r.float3(object.body.linearVelocity) && r.float3(object.body.angularVelocity) &&
                  r.boolean(object.body.sleeping) && r.float3(object.localCenterOfMassMeters)))
                return corrupt(r);
        }
        for (const GameWorldObjectState& object : world.objects) {
            if (object.hasBody && !seen.contains(object.id))
                return fail(error, "save has no body state for object " + std::to_string(object.id)), std::nullopt;
        }
        if (!r.finish()) return corrupt(r);
    }

    if (const auto* script = section(kScriptSection)) {
        if (script->size() > limits_.maximumScriptStateBytes) return fail(error, "script state is over the save limit"), std::nullopt;
        data.scriptState = *script;
    }
    for (const auto& [name, bytes] : document.sections) {
        if (name.starts_with(kReservedPrefix)) {
            if (name != kMetaSection && name != kWorldSection && name != kVoxelSection && name != kPhysicsSection &&
                name != kScriptSection)
                return fail(error, "save has an unknown engine section '" + name + "'"), std::nullopt;
            continue;
        }
        data.gameSections.emplace(name, bytes);
    }
    return data;
}

std::optional<std::vector<std::byte>> GameSaveCodec::encode(
    const GameSaveData& data, GameSaveStats* stats, std::string* error) const {
    auto document = to_document(data, stats, error);
    if (!document) return std::nullopt;
    return encode_save_game_document(*document);
}

std::optional<GameSaveData> GameSaveCodec::decode(
    std::span<const std::byte> bytes, SaveGameReadReport* report, std::string* error) const {
    auto document = store_.decode(bytes, report, error);
    if (!document) return std::nullopt;
    return from_document(*document, error);
}

bool GameSaveCodec::write_file(
    const std::filesystem::path& path, const GameSaveData& data, GameSaveStats* stats, std::string* error) const {
    GameSaveStats local;
    auto document = to_document(data, &local, error);
    if (!document) return false;
    std::uint64_t written = 0U;
    if (!store_.write_atomic(path, std::move(*document), error, &written)) return false;
    local.fileBytes = written;
    if (stats) *stats = std::move(local);
    return true;
}

std::optional<GameSaveData> GameSaveCodec::read_file(
    const std::filesystem::path& path, const SaveGameReadOptions& options, SaveGameReadReport* report,
    std::string* error) const {
    auto document = store_.read(path, options, report, error);
    if (!document) return std::nullopt;
    return from_document(*document, error);
}

} // namespace dve
